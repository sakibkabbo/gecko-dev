/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

const ONEOFF_URLBAR_PREF = "browser.urlbar.oneOffSearches";

function repeat(limit, func) {
  for (let i = 0; i < limit; i++) {
    func(i);
  }
}

function is_selected(index) {
  is(gURLBar.popup.richlistbox.selectedIndex, index, `Item ${index + 1} should be selected`);
  // Also check the "selected" attribute, to ensure it is not a "fake" selection
  // due to binding misbehaviors.
  ok(gURLBar.popup.richlistbox.selectedItem.hasAttribute("selected"),
     `Item ${index + 1} should have the "selected" attribute`);

  // This is true because although both the listbox and the one-offs can have
  // selections, the test doesn't check that.
  is(gURLBar.popup.oneOffSearchButtons.selectedButton, null,
     "A result is selected, so the one-offs should not have a selection");
}

function is_selected_one_off(index) {
  is(gURLBar.popup.oneOffSearchButtons.selectedButtonIndex, index,
     "Expected one-off button should be selected");

  // This is true because although both the listbox and the one-offs can have
  // selections, the test doesn't check that.
  is(gURLBar.popup.richlistbox.selectedIndex, -1,
     "A one-off is selected, so the listbox should not have a selection");
}

add_task(async function() {
  let maxResults = Services.prefs.getIntPref("browser.urlbar.maxRichResults");
  Services.prefs.setBoolPref(ONEOFF_URLBAR_PREF, true);
  let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, "about:mozilla");
  registerCleanupFunction(async function() {
    await PlacesUtils.history.clear();
    Services.prefs.clearUserPref(ONEOFF_URLBAR_PREF);
    BrowserTestUtils.removeTab(tab);
  });

  let visits = [];
  repeat(maxResults, i => {
    visits.push({
      uri: makeURI("http://example.com/autocomplete/?" + i),
    });
  });
  await PlacesTestUtils.addVisits(visits);

  await promiseAutocompleteResultPopup("example.com/autocomplete");
  await waitForAutocompleteResultAt(maxResults - 1);

  let popup = gURLBar.popup;
  let results = popup.richlistbox.itemChildren;
  is(results.length, maxResults,
     "Should get maxResults=" + maxResults + " results");
  is_selected(0);

  info("Key Down to select the next item");
  EventUtils.synthesizeKey("KEY_ArrowDown");
  is_selected(1);

  info("Key Down maxResults-1 times should select the first one-off");
  repeat(maxResults - 1, () => EventUtils.synthesizeKey("KEY_ArrowDown"));
  is_selected_one_off(0);

  info("Key Down numButtons-1 should select the last one-off");
  let numButtons =
    gURLBar.popup.oneOffSearchButtons.getSelectableButtons(true).length;
  repeat(numButtons - 1, () => EventUtils.synthesizeKey("KEY_ArrowDown"));
  is_selected_one_off(numButtons - 1);

  info("Key Down twice more should select the second result");
  repeat(2, () => EventUtils.synthesizeKey("KEY_ArrowDown"));
  is_selected(1);

  info("Key Down maxResults + numButtons times should wrap around");
  repeat(maxResults + numButtons,
         () => EventUtils.synthesizeKey("KEY_ArrowDown"));
  is_selected(1);

  info("Key Up maxResults + numButtons times should wrap around the other way");
  repeat(maxResults + numButtons, () => EventUtils.synthesizeKey("KEY_ArrowUp"));
  is_selected(1);

  info("Page Up will go up the list, but not wrap");
  EventUtils.synthesizeKey("KEY_PageUp");
  is_selected(0);

  info("Page Up again will wrap around to the end of the list");
  EventUtils.synthesizeKey("KEY_PageUp");
  is_selected(maxResults - 1);

  EventUtils.synthesizeKey("KEY_Escape");
  await promisePopupHidden(gURLBar.popup);
});
