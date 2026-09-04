#include "Flenser.h"

/**
	The generator: the wheel with the plugin's own lamp behind it.

	**This file is listed directly in the FlenserSource target, not in
	flenser_core.** Both plugins share the class; what they do not share is
	the `CFFGLPluginInfo` below, and putting either registration in the shared
	library would register both plugins into both bundles.

	It is also why the shared library is an OBJECT library rather than a
	STATIC one. `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in an archive the
	linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Flenser.bundle/Contents/MacOS/Flenser | grep plugMain
*/
namespace
{
class FlenserSource : public FlenserPlugin
{
public:
	FlenserSource() :
		FlenserPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FlenserSource >,                             // Create method
	"FL01",                                                     // Plugin unique ID of maximum length 4
	"Flenser",                                                  // Plugin name
	2,                                                          // API major version number
	1,                                                          // API minor version number
	0,                                                          // Plugin major version number
	1,                                                          // Plugin minor version number
	FF_SOURCE,                                                  // Plugin type
	"Oil, water and dye on an overhead projector.\n\nOil in dyed water does not mix. It forms rounded cells that press on each other and join with a fillet where they meet, and each cell is a dye filter - so overlapping cells multiply rather than add, and cyan over magenta is blue, not white.\n\nThe meniscus at every edge is a lens: it displaces what is behind it, splits it by wavelength, and throws a bright caustic just inside a dark rim. Behind all of it is a Fresnel condenser with a visible hot spot, a colour temperature and a round gate.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Flenser FFGL source"                                       // About
);

extern "C" const char* FlenserSourceBuildStamp()
{
	return "flenser " FLENSER_VERSION " source, built " __DATE__ " " __TIME__;
}
