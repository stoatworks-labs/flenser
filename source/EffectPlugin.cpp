#include "Flenser.h"

/**
	The effect: the same wheel, with the incoming clip as the lamp.

	Read the note in SourcePlugin.cpp on why this file is listed directly in
	its own target rather than in `flenser_core`. The short version is that
	the `CFFGLPluginInfo` below is the one thing the two plugins must NOT
	share.

	The plugin ID differs from the source's, and it has to: Resolume keys a
	saved composition's effect to that ID, so two plugins sharing one would
	make a composition ambiguous about which of them it meant.

	It is called "Flenser Lamp" rather than "Flenser Effect" because that is
	what it does -- the clip is put where the projector's lamp was, and
	everything the wheel does to light it now does to the footage.
*/
namespace
{
class FlenserEffect : public FlenserPlugin
{
public:
	FlenserEffect() :
		FlenserPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FlenserEffect >,                             // Create method
	"FL02",                                                     // Plugin unique ID of maximum length 4
	"Flenser Lamp",                                             // Plugin name
	2,                                                          // API major version number
	1,                                                          // API minor version number
	0,                                                          // Plugin major version number
	1,                                                          // Plugin minor version number
	FF_EFFECT,                                                  // Plugin type
	"The clip projected through oil, water and dye.\n\nOil in dyed water does not mix. It forms rounded cells that press on each other and join with a fillet where they meet, and each cell is a dye filter - so overlapping cells multiply rather than add, and cyan over magenta is blue, not white.\n\nThe meniscus at every edge is a lens: it displaces what is behind it, splits it by wavelength, and throws a bright caustic just inside a dark rim. Behind all of it is a Fresnel condenser with a visible hot spot, a colour temperature and a round gate.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Flenser FFGL effect"                                       // About
);

extern "C" const char* FlenserEffectBuildStamp()
{
	return "flenser " FLENSER_VERSION " effect, built " __DATE__ " " __TIME__;
}
