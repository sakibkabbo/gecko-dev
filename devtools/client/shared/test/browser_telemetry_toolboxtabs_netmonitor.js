/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const TEST_URI = "data:text/html;charset=utf-8," +
  "<p>browser_telemetry_toolboxtabs_netmonitor.js</p>";

// Because we need to gather stats for the period of time that a tool has been
// opened we make use of setTimeout() to create tool active times.
const TOOL_DELAY = 200;

add_task(async function() {
  await addTab(TEST_URI);
  startTelemetry();

  await openAndCloseToolbox(2, TOOL_DELAY, "netmonitor");
  checkResults();

  gBrowser.removeCurrentTab();
});

function checkResults() {
  // For help generating these tests use generateTelemetryTests("DEVTOOLS_NETMONITOR_")
  // here.
  checkTelemetry("DEVTOOLS_NETMONITOR_OPENED_COUNT", "", {0: 2, 1: 0}, "array");
  checkTelemetry("DEVTOOLS_NETMONITOR_TIME_ACTIVE_SECONDS", "", null, "hasentries");
}
