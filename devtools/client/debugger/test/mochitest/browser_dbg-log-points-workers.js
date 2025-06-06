/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

/*
 * Tests that log points in a worker are correctly logged to the console
 */

"use strict";

add_task(async function () {
  Services.prefs.setBoolPref("devtools.toolbox.splitconsole.open", true);

  const dbg = await initDebugger("doc-windowless-workers.html");

  await waitForSource(dbg, "simple-worker.js");
  await selectSource(dbg, "simple-worker.js");

  await altClickElement(dbg, "gutterElement", 4);
  await waitForBreakpoint(dbg, "simple-worker.js", 4);

  await getDebuggerSplitConsole(dbg);
  await hasConsoleMessage(dbg, "timer");
  await waitForConsoleMessageLink(dbg.toolbox, "timer", "simple-worker.js:4:9");
  ok(true, "message includes the url and linenumber");
});
