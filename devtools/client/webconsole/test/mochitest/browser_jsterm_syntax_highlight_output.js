/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const TEST_URI =
  "data:text/html;charset=utf-8,Test syntax highlighted output";

add_task(async function() {
  await pushPref("devtools.webconsole.jsterm.codeMirror", true);
  await performTests();

  await pushPref("devtools.webconsole.jsterm.codeMirror", false);
  await performTests();
});

async function performTests() {
  const hud = await openNewTabAndConsole(TEST_URI);
  const jsterm = hud.jsterm;

  // Syntax highlighting is implemented with a Custom Element:
  ok(hud.iframeWindow.customElements.get("syntax-highlighted"), "Custom Element exists");

  // Check that we syntax highlight output to look like the inputed text.
  // See Bug 1463669.
  const onMessage = waitForMessage(hud, `var a = 'str';`);
  jsterm.execute("var a = 'str';");
  const message = await onMessage;
  const highlighted = message.node.querySelectorAll("syntax-highlighted");
  let expectedMarkup;
  if (Services.prefs.getBoolPref("devtools.webconsole.jsterm.codeMirror")) {
    /* eslint-disable-next-line max-len */
    expectedMarkup = `<syntax-highlighted class="cm-s-mozilla"><span class="cm-keyword">var</span> <span class="cm-def">a</span> <span class="cm-operator">=</span> <span class="cm-string">'str'</span>;</syntax-highlighted>`;
  } else {
    expectedMarkup = `<syntax-highlighted>var a = 'str';</syntax-highlighted>`;
  }
  is(highlighted.length, 1, "1 syntax highlighted tag");
  is(highlighted[0].outerHTML, expectedMarkup, "got expected html");
}
