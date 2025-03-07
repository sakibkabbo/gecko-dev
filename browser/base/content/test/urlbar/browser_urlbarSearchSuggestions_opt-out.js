/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable mozilla/no-arbitrary-setTimeout */
// The order of the tests here matters!

const SUGGEST_ALL_PREF = "browser.search.suggest.enabled";
const SUGGEST_URLBAR_PREF = "browser.urlbar.suggest.searches";
const CHOICE_PREF = "browser.urlbar.userMadeSearchSuggestionsChoice";
const TIMES_PREF = "browser.urlbar.timesBeforeHidingSuggestionsHint";
const TEST_ENGINE_BASENAME = "searchSuggestionEngine.xml";
const ONEOFF_PREF = "browser.urlbar.oneOffSearches";
const NO_RESULTS_TIMEOUT_MS = 500;

add_task(async function prepare() {
  let engine = await SearchTestUtils.promiseNewSearchEngine(
    getRootDirectory(gTestPath) + TEST_ENGINE_BASENAME);
  let oldCurrentEngine = Services.search.defaultEngine;
  Services.search.defaultEngine = engine;
  let suggestionsEnabled = Services.prefs.getBoolPref(SUGGEST_URLBAR_PREF);
  let defaults = Services.prefs.getDefaultBranch("browser.urlbar.");
  let searchSuggestionsDefault = defaults.getBoolPref("suggest.searches");
  defaults.setBoolPref("suggest.searches", true);
  let suggestionsChoice = Services.prefs.getBoolPref(CHOICE_PREF);
  Services.prefs.setBoolPref(CHOICE_PREF, false);
  let oneOffs = Services.prefs.getBoolPref(ONEOFF_PREF);
  Services.prefs.setBoolPref(ONEOFF_PREF, true);
  registerCleanupFunction(async function() {
    defaults.setBoolPref("suggest.searches", searchSuggestionsDefault);
    Services.search.defaultEngine = oldCurrentEngine;
    Services.prefs.clearUserPref(SUGGEST_ALL_PREF);
    Services.prefs.setBoolPref(SUGGEST_URLBAR_PREF, suggestionsEnabled);
    Services.prefs.setBoolPref(CHOICE_PREF, suggestionsChoice);
    Services.prefs.setBoolPref(ONEOFF_PREF, oneOffs);
    // Make sure the popup is closed for the next test.
    gURLBar.blur();
    Assert.ok(!gURLBar.popup.popupOpen, "popup should be closed");
  });
});

add_task(async function focus() {
  // Focusing the urlbar should open the popup in order to show the
  // notification.
  setupVisibleHint();
  gURLBar.blur();
  let popupPromise = promisePopupShown(gURLBar.popup);
  focusAndSelectUrlBar(true);
  await popupPromise;
  Assert.ok(gURLBar.popup.popupOpen, "popup should be open");
  // There are no results, so wait a bit for *nothing* to appear.
  await new Promise(resolve => setTimeout(resolve, NO_RESULTS_TIMEOUT_MS));
  assertVisible(true);
  assertFooterVisible(false);
  Assert.equal(gURLBar.popup.matchCount, 0, "popup should have no results");

  // Start searching.
  EventUtils.sendString("rnd");
  await promiseSearchComplete();
  await waitForAutocompleteResultAt(0);
  Assert.ok(suggestionsPresent());
  assertVisible(true);
  assertFooterVisible(true);

  // Check the Change Options link.
  let changeOptionsLink = document.getAnonymousElementByAttribute(gURLBar.popup, "id", "search-suggestions-change-settings");
  let prefsPromise = BrowserTestUtils.waitForLocationChange(gBrowser, "about:preferences#search");
  changeOptionsLink.click();
  await prefsPromise;
  Assert.ok(!gURLBar.popup.popupOpen, "popup should be closed");
  // The preferences page does fancy stuff with focus, ensure to unload it.
  await BrowserTestUtils.loadURI(gBrowser.selectedBrowser, "about:blank");
});

add_task(async function click_on_focused() {
  // Even if the location bar is already focused, we should still show the popup
  // and the notification on click.
  setupVisibleHint();
  gURLBar.blur();
  // Won't show the hint since it's not user initiated.
  gURLBar.focus();
  await new Promise(resolve => setTimeout(resolve, 1000));
  Assert.ok(!gURLBar.popup.popupOpen, "popup should be closed");
  Assert.ok(gURLBar.focused, "The input field should be focused");

  let popupPromise = promisePopupShown(gURLBar.popup);
  EventUtils.synthesizeMouseAtCenter(gURLBar.inputField, {});
  await popupPromise;
  Assert.ok(gURLBar.popup.popupOpen, "popup should be open");
  // There are no results, so wait a bit for *nothing* to appear.
  await new Promise(resolve => setTimeout(resolve, NO_RESULTS_TIMEOUT_MS));
  assertVisible(true);
  assertFooterVisible(false);
  Assert.equal(gURLBar.popup.matchCount, 0, "popup should have no results");
  gURLBar.blur();
  Assert.ok(!gURLBar.popup.popupOpen, "popup should be closed");
});

add_task(async function new_tab() {
  // Opening a new tab when the urlbar is unfocused, should focus it but not
  // open the popup.
  setupVisibleHint();
  gURLBar.blur();
  // openNewForegroundTab doesn't focus the urlbar.
  await BrowserTestUtils.synthesizeKey("t", { accelKey: true }, gBrowser.selectedBrowser);
  await new Promise(resolve => setTimeout(resolve, NO_RESULTS_TIMEOUT_MS));
  Assert.ok(!gURLBar.popup.popupOpen, "popup should be closed");
  BrowserTestUtils.removeTab(gBrowser.selectedTab);
});

add_task(async function privateWindow() {
  // Since suggestions are disabled in private windows, the notification should
  // not appear even when suggestions are otherwise enabled.
  setupVisibleHint();
  let win = await BrowserTestUtils.openNewBrowserWindow({ private: true });
  await promiseAutocompleteResultPopup("foo", win);
  // There are no results, so wait a bit for *nothing* to appear.
  await new Promise(resolve => setTimeout(resolve, NO_RESULTS_TIMEOUT_MS));
  assertVisible(false, win);
  assertFooterVisible(true, win);
  win.gURLBar.blur();
  await BrowserTestUtils.closeWindow(win);
});

add_task(async function enableOutsideNotification() {
  // Setting the suggest.searches pref outside the notification (e.g., by
  // ticking the checkbox in the preferences window) should hide it.
  setupVisibleHint();
  Services.prefs.setBoolPref(SUGGEST_URLBAR_PREF, false);
  await promiseAutocompleteResultPopup("foo");
  await waitForAutocompleteResultAt(0);
  assertVisible(false);
  assertFooterVisible(true);
});

add_task(async function userMadeChoice() {
  // If the user made a choice already, he should not see the hint.
  setupVisibleHint();
  Services.prefs.setBoolPref(CHOICE_PREF, true);
  await promiseAutocompleteResultPopup("foo");
  await waitForAutocompleteResultAt(0);
  assertVisible(false);
  assertFooterVisible(true);
});

function setupVisibleHint() {
  Services.prefs.clearUserPref(TIMES_PREF);
  Services.prefs.setBoolPref(SUGGEST_ALL_PREF, true);
  // Toggle to reset the whichNotification cache.
  Services.prefs.setBoolPref(SUGGEST_URLBAR_PREF, false);
  Services.prefs.setBoolPref(SUGGEST_URLBAR_PREF, true);
}

function assertVisible(visible, win = window) {
  let style =
    win.getComputedStyle(win.gURLBar.popup.searchSuggestionsNotification);
  let check = visible ? "notEqual" : "equal";
  Assert[check](style.display, "none");
}
function assertFooterVisible(visible, win = window) {
  let style = win.getComputedStyle(win.gURLBar.popup.footer);
  Assert.equal(style.visibility, visible ? "visible" : "collapse");
}
