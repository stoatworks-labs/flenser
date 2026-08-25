#pragma once

#include <string>

/**
	Logging for a plugin that lives inside somebody else's process.

	A small member of the fleet's `diag` family. The rest of the repos get a
	rotating log, a crash report and a diagnostics bundle; an FFGL plugin gets
	only the log, for two reasons:

	- **No crash handler.** A plugin loaded into Resolume must not install a
	  process-wide signal handler. It would intercept faults that are not ours
	  and interfere with the host's own handling. A plugin has no business
	  deciding what happens when Resolume dies.
	- **No bundle command.** There is no UI to hang one off -- a plugin is a
	  list of sliders in someone else's inspector.

	What it covers is the failures that actually happen, all of which look
	identical from the operator's side ("it does nothing") with no message
	anywhere:

	- **A shader would not compile**, so `InitGL` returns `FF_FAIL`. The GL
	  vendor, renderer and version strings are logged next to it, because a
	  shader that builds on one machine and not on another is a driver answer
	  and not a source answer. This one is the reason the file exists: the oil
	  pass is *assembled* from three strings, so the line number in a driver's
	  compile log refers to a file that does not exist on disk and the log is
	  the only place the assembled text's failure is recorded.
	- **A buffer the driver would not allocate.** Logged with the size that
	  was asked for, because the answer is almost always the canvas.
	- **The preset dropping back to Custom**, which is a state change an
	  operator can be surprised by and which happens once rather than per
	  frame.

	    ~/Library/Logs/flenser/flenser.YYYY-MM-DD.log

	`FLENSER_LOG_DIR` overrides the directory, which is what the harness uses
	to keep a test run out of the real log.
*/
namespace flenser::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace flenser::diag
