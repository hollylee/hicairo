/* WARNING: Autogenerated file - see ./make-cairo-boilerplate-constructors.sh! */

#include "cairo-boilerplate-private.h"

void _cairo_boilerplate_register_all (void);

extern void _register_builtin (void);
extern void _register_drm (void);
extern void _register_egl (void);
extern void _register_gl (void);
extern void _register_script (void);
extern void _register_ps (void);
extern void _register_pdf (void);
extern void _register_svg (void);

void
_cairo_boilerplate_register_all (void)
{
    _register_builtin ();
    _register_drm ();
    _register_egl ();
    _register_gl ();
    _register_script ();
    _register_ps ();
    _register_pdf ();
    _register_svg ();
}
