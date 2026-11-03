"""Texture capabilities used at generation; no device is contacted for an explicit target."""
from porter.upstream import BuildError

# A small, conservative mapping. Unknown model names need an explicit GPU family.
TARGETS = {'adreno': 'bc', 'xclipse': 'bc', 'nvidia': 'bc',
           'mali': 'astc', 'immortalis': 'astc', 'powervr': 'astc'}
MODELS = {'tb323fu': 'adreno', 'v2405a': 'immortalis', 'pd2405': 'immortalis',
          'dimensity-9400': 'immortalis'}
CHOICES = ('auto', *TARGETS, *MODELS)


def family(gpu):
    return next((f for f in TARGETS if f in gpu.lower()), None)


def select(target='auto', *, adb=None):
    target = target.strip().lower()
    if target != 'auto':
        f = MODELS.get(target, target)
        if f not in TARGETS: raise BuildError(f'unknown target model {target!r}; choose --target-gpu adreno, xclipse, mali, immortalis or powervr')
        return TARGETS[f]
    if adb is not None:
        from porter.adb import AdbError
        try:
            adb.pick()
            bc = adb.bc_textures()
            if bc is True: return 'bc'
            if bc is False and adb.astc_textures() is True: return 'astc'
        except AdbError as e:
            raise BuildError(f'cannot select the texture target ({e}); choose --target-gpu for an offline port') from e
    raise BuildError('no known texture target: connect one device or choose --target-gpu (model or GPU family) for an offline port')
