// Upcoming-command prediction, adapted from nxnative c646d43 pso_predict_rest.
// Walk a private state copy. Stop at changed targets or memory/compute work:
// their actual attachment formats and shader-view variants are replay decisions.
// Guessed variants can only populate another cache key; a real draw recomputes
// its own exact key and waits normally if it was not prepared.
static void predict_program(vk_replay_state *state, const p_bind_program *bind) {
    uint32_t mask = bind->stages ? (uint32_t)bind->stages & 0x3f : 0x3f;
    for (int st = 0; st < 6; st++) if (mask >> st & 1) state->sh[st] = NULL;
    state->missing_stages &= ~mask; state->prog = bind->prog;
    if (bind->prog) for (unsigned i = 0; i < 6; i++) {
        uint8_t bytes[8]; if (!prog_key(bind->prog, i, bytes)) break;
        uint64_t key = key64(bytes); int stage = vk_shader_stage(key);
        if (stage < 0) { state->missing_stages |= mask; continue; }
        if (stage > 5 || !(mask >> stage & 1)) continue;
        vk_shader_prefetch(key, stage); state->sh[stage] = vk_shader_cached(key, stage);
        if (!state->sh[stage]) state->missing_stages |= 1u << stage;
    }
}
static int pipeline_predict_rest(const pkey *current, int budget) {
    if (!g_pipe_predict_on || !g_predict_next || g_predict_depth > 16 || g_var_now || g_spv_alt || g_tracing || g_tf_on || g_rc_skip) return 0;
    // The replay has one owner. Keep its large private shadow off guest stacks.
    static vk_replay_state state; state = S;
    const uint8_t *p = g_predict_next, *returns[16]; unsigned depth = g_predict_depth;
    memcpy(returns, g_predict_returns, depth * sizeof *returns);
    uint32_t changed_views = 0; int admitted = 0;
    for (unsigned guard = 0; p && guard < 8192 && admitted < budget; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); if (h.size < 8 || h.size > (1u << 24)) break;
        const void *a = p + 8; uint32_t bytes = h.size - 8;
        switch (h.op) {
        case OP_END: if (!depth) return admitted; p = returns[--depth]; continue;
        case OP_JUMP: if (bytes < 8) return admitted; memcpy(&p, a, 8); continue;
        case OP_CALL: if (bytes < 8 || depth == 16) return admitted; returns[depth++] = p + h.size; memcpy(&p, a, 8); continue;
        case OP_BEGIN: break;
        case OP_SET_RT: if (bytes < sizeof(p_set_rt) || g_x_nokeep || !rt_same_state(&state, a)) return admitted; break;
        case OP_BIND_PROGRAM: if (bytes < sizeof(p_bind_program)) return admitted; predict_program(&state, a); break;
        case OP_BIND_STATE: {
            if (bytes < 4) return admitted; const p_bind_state *b = a;
            static const unsigned sizes[] = {sizeof(blend_st), sizeof(color_st), sizeof(chmask_st), sizeof(ms_st), sizeof(poly_st), sizeof(ds_st)};
            if (b->kind < 0 || b->kind >= 6 || bytes - 4 < sizes[b->kind]) return admitted;
            switch (b->kind) {
            case 0: { blend_st v; memcpy(&v, b->bytes, sizeof v); if (v.target < 8) state.blend[v.target] = v; break; }
            case 1: memcpy(&state.color, b->bytes, sizeof state.color); break;
            case 2: memcpy(&state.chmask, b->bytes, sizeof state.chmask); break;
            case 3: memcpy(&state.ms, b->bytes, sizeof state.ms); break;
            case 4: memcpy(&state.poly, b->bytes, sizeof state.poly); break;
            case 5: memcpy(&state.ds, b->bytes, sizeof state.ds); break;
            } break;
        }
        case OP_BIND_VATTR: {
            if (bytes < 8) return admitted; const p_bind_vattr *v = a;
            if (v->first < 0 || v->n < 0 || v->first > 16 || v->n > 16 - v->first || bytes - 8 < (unsigned)v->n * sizeof *v->st) return admitted;
            memset(state.vattr, 0, sizeof state.vattr); memcpy(state.vattr + v->first, v->st, (size_t)v->n * sizeof *v->st); break;
        }
        case OP_BIND_VSTREAM: {
            if (bytes < 8) return admitted; const p_bind_vstream *v = a;
            if (v->first < 0 || v->n < 0 || v->first > 16 || v->n > 16 - v->first || bytes - 8 < (unsigned)v->n * sizeof *v->st) return admitted;
            memcpy(state.vstream + v->first, v->st, (size_t)v->n * sizeof *v->st); break;
        }
        case OP_BIND_VBUF: {
            if (bytes < 8) return admitted; const p_bind_vbuf *v = a;
            if (v->first < 0 || v->n < 0 || v->first > 16 || v->n > 16 - v->first || bytes - 8 < (unsigned)v->n * sizeof *v->r) return admitted;
            memcpy(state.vbuf + v->first, v->r, (size_t)v->n * sizeof *v->r); break;
        }
        case OP_DEPTH_CLAMP: if (bytes < sizeof(p_depth_clamp)) return admitted; state.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_RAST_DISCARD: if (bytes < sizeof(p_depth_clamp)) return admitted; state.rast_discard = ((const p_depth_clamp *)a)->on; break;
        case OP_PRIM_RESTART: if (bytes < sizeof(p_prim_restart)) return admitted; state.prim_restart = ((const p_prim_restart *)a)->on; break;
        case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_BIND_UBO: case OP_BIND_SSBO:
            changed_views = 0x3f; break;   // no deferred-handle or uniform-memory reads during prediction
        case OP_DRAW: case OP_DRAW_IDX: {
            if (bytes < 4 || !state.sh[0] || state.sh[3] || state.sh[4] || (state.missing_stages & 0x1f) || state.rast_discard || state.sh[0]->xfb || (state.sh[2] && !V.has_gs)) break;
            int prim; memcpy(&prim, a, 4); int indexed = h.op == OP_DRAW_IDX || prim == 7;
            if (prim == 0xe) break;   // tessellation is outside this predictor
            pkey key; pipeline_key(&key, &state, prim == 7 ? 4 : prim, indexed, 0);
            for (int st = 0; st < 3; st++) if (!(changed_views >> st & 1) && key.key[st] == current->key[st]) { key.um[st] = current->um[st]; key.im[st] = current->im[st]; }
            vkshader_t *fs = state.sh[1]; const vkimg_t *dim = state.daspect & VK_IMAGE_ASPECT_DEPTH_BIT ? P_att[8].im : NULL;
            key.fdz |= g_fdz && !P_depth_copied && dim && fs && !fs->depth_repl && key.dt && !key.bias_en && dim->fdz_frame && g_tex_frame + 1 - dim->fdz_frame <= 2;
            VkDescriptorSetLayout sets[5] = {0}; int nsets = 0;
            for (int st = 0; st < 3; st++) if (state.sh[st]) { sets[state.sh[st]->set] = state.sh[st]->dsl; if (state.sh[st]->set + 1 > nsets) nsets = state.sh[st]->set + 1; }
            admitted += pipeline_predict_kick(&key, vk_pipeline_layout(sets, nsets), state.sh); break;
        }
        case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_VP_SWIZZLE:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;   // dynamic state / already rebased handles
        default: return admitted;   // no speculation across a memory, pass or synchronization boundary
        }
        p += h.size;
    }
    return admitted;
}
