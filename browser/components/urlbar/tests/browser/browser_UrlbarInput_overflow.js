/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

async function testVal(aExpected, overflowSide = "") {
  info(`Testing ${aExpected}`);
  URLBarSetURI(makeURI(aExpected));

  Assert.equal(gURLBar.selectionStart, gURLBar.selectionEnd,
    "Selection sanity check");

  gURLBar.focus();
  Assert.equal(document.activeElement, gURLBar.inputField, "URL Bar should be focused");
  Assert.equal(gURLBar.valueFormatter.scheme.value, "", "Check the scheme value");
  Assert.equal(getComputedStyle(gURLBar.valueFormatter.scheme).visibility, "hidden",
               "Check the scheme box visibility");

  gURLBar.blur();
  await window.promiseDocumentFlushed(() => {});
  // The attribute doesn't always change, so we can't use waitForAttribute.
  await TestUtils.waitForCondition(
    () => gURLBar.getAttribute("textoverflow") === overflowSide);

  let scheme = aExpected.match(/^([a-z]+:\/{0,2})/)[1];
  // We strip http, so we should not show the scheme for it.
  if (scheme == "http://" && Services.prefs.getBoolPref("browser.urlbar.trimURLs", true))
    scheme = "";

  Assert.equal(gURLBar.valueFormatter.scheme.value, scheme, "Check the scheme value");
  let isOverflowed = gURLBar.inputField.scrollWidth > gURLBar.inputField.clientWidth;
  Assert.equal(isOverflowed, !!overflowSide, "Check The input field overflow");
  Assert.equal(gURLBar.getAttribute("textoverflow"), overflowSide,
               "Check the textoverflow attribute");
  if (overflowSide) {
    let side = gURLBar.inputField.scrollLeft == 0 ? "end" : "start";
    Assert.equal(side, overflowSide, "Check the overflow side");
    Assert.equal(getComputedStyle(gURLBar.valueFormatter.scheme).visibility,
                 scheme && isOverflowed && overflowSide == "start" ? "visible" : "hidden",
                 "Check the scheme box visibility");
  }
}

add_task(async function() {
  // We use a new tab for the test to be sure all the tab switching and loading
  // is complete before starting, otherwise onLocationChange for this tab could
  // override the value we set with an empty value.
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser);
  registerCleanupFunction(function() {
    URLBarSetURI();
    BrowserTestUtils.removeTab(tab);
  });

  let lotsOfSpaces = "%20".repeat(200);

  // اسماء.شبكة
  let rtlDomain = "\u0627\u0633\u0645\u0627\u0621\u002e\u0634\u0628\u0643\u0629";

  // Mix the direction of the tests to cover more cases, and to ensure the
  // textoverflow attribute changes every time, because tewtVal waits for that.
  await testVal(`https://mozilla.org/${lotsOfSpaces}/test/`, "end");
  await testVal(`https://mozilla.org/`);
  await testVal(`https://${rtlDomain}/${lotsOfSpaces}/test/`, "start");

  await testVal(`ftp://mozilla.org/${lotsOfSpaces}/test/`, "end");
  await testVal(`ftp://${rtlDomain}/${lotsOfSpaces}/test/`, "start");
  await testVal(`ftp://mozilla.org/`);
  await testVal(`http://${rtlDomain}/${lotsOfSpaces}/test/`, "start");
  await testVal(`http://mozilla.org/`);
  await testVal(`http://mozilla.org/${lotsOfSpaces}/test/`, "end");
  await testVal(`https://${rtlDomain}:80/${lotsOfSpaces}/test/`, "start");

  info("Test with formatting disabled");
  await SpecialPowers.pushPrefEnv({set: [
    ["browser.urlbar.formatting.enabled", false],
    ["browser.urlbar.trimURLs", false],
  ]});

  await testVal(`https://mozilla.org/`);
  await testVal(`https://${rtlDomain}/${lotsOfSpaces}/test/`, "start");
  await testVal(`https://mozilla.org/${lotsOfSpaces}/test/`, "end");

  info("Test with trimURLs disabled");
  await testVal(`http://${rtlDomain}/${lotsOfSpaces}/test/`, "start");
});
