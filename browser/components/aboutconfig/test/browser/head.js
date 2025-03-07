/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

ChromeUtils.import("resource://gre/modules/Preferences.jsm", this);

// List of default preferences that can be used for tests, chosen because they
// have little or no side-effects when they are modified for a brief time. If
// any of these preferences are removed or their default state changes, just
// update the constant to point to a different preference with the same default.
const PREF_BOOLEAN_DEFAULT_TRUE = "accessibility.typeaheadfind.manual";
const PREF_BOOLEAN_USERVALUE_TRUE = "browser.dom.window.dump.enabled";
const PREF_NUMBER_DEFAULT_ZERO = "accessibility.typeaheadfind.casesensitive";
const PREF_STRING_DEFAULT_EMPTY = "browser.helperApps.neverAsk.openFile";
const PREF_STRING_DEFAULT_NOTEMPTY = "accessibility.typeaheadfind.soundURL";
const PREF_STRING_DEFAULT_NOTEMPTY_VALUE = "beep";
const PREF_STRING_LOCALIZED_MISSING = "gecko.handlerService.schemes.irc.1.name";

class AboutConfigRowTest {
  constructor(element) {
    this.element = element;
  }

  querySelector(selector) {
    return this.element.querySelector(selector);
  }

  get name() {
    return this.querySelector("td").textContent;
  }

  get value() {
    return this.querySelector("td.cell-value").textContent;
  }

  /**
   * Text input field when the row is in edit mode.
   */
  get valueInput() {
    return this.querySelector("td.cell-value input");
  }

  /**
   * This is normally "edit" or "toggle" based on the preference type, or "save"
   * when the row is in edit mode.
   */
  get editColumnButton() {
    return this.querySelector("td.cell-edit > button");
  }

  /**
   * This can be "reset" or "delete" based on whether a default exists.
   */
  get resetColumnButton() {
    return this.querySelector("td:last-child > button");
  }

  hasClass(className) {
    return this.element.classList.contains(className);
  }
}

class AboutConfigTest {
  static withNewTab(testFn, options = {}) {
    return BrowserTestUtils.withNewTab({
      gBrowser,
      url: "chrome://browser/content/aboutconfig/aboutconfig.html",
    }, async browser => {
      let scope = new this(browser);
      await scope.setupNewTab(options);
      await testFn.call(scope);
    });
  }

  constructor(browser) {
    this.document = browser.contentDocument;
  }

  async setupNewTab(options) {
    await this.document.l10n.ready;
    if (!options.dontBypassWarning) {
      this.document.querySelector("button").click();
    }
  }

  /**
   * Array of AboutConfigRowTest objects, one for each row in the main table.
   */
  get rows() {
    let elements = this.document.getElementById("prefs")
                                .getElementsByTagName("tr");
    return Array.map(elements, element => new AboutConfigRowTest(element));
  }

  /**
   * Returns the AboutConfigRowTest object for the row in the main table which
   * corresponds to the given preference name, or undefined if none is present.
   */
  getRow(name) {
    return this.rows.find(row => row.name == name);
  }

  /**
   * Performs a new search using the dedicated textbox. This also makes sure
   * that the list of preferences displayed is up to date.
   */
  search(value = "") {
    let search = this.document.getElementById("search");
    search.value = value;
    search.focus();
    EventUtils.sendKey("return");
  }
}
