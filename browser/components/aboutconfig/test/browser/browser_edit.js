/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

add_task(async function setup() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["test.aboutconfig.modify.boolean", true],
      ["test.aboutconfig.modify.number", 1337],
      ["test.aboutconfig.modify.string", "the answer to the life the universe and everything"],
    ],
  });

  registerCleanupFunction(() => {
    Services.prefs.clearUserPref(PREF_BOOLEAN_DEFAULT_TRUE);
    Services.prefs.clearUserPref(PREF_NUMBER_DEFAULT_ZERO);
    Services.prefs.clearUserPref(PREF_STRING_DEFAULT_EMPTY);
  });
});

add_task(async function test_add_user_pref() {
  await AboutConfigTest.withNewTab(async function() {
    Assert.ok(!Services.prefs.getChildList("").find(pref => pref == "testPref"));

    for (let [buttonSelector, expectedValue] of [
      [".add-true", true],
      [".add-false", false],
      [".add-Number", 0],
      [".add-String", ""],
    ]) {
      this.search("testPref");
      this.document.querySelector("#prefs button" + buttonSelector).click();
      Assert.ok(Services.prefs.getChildList("").find(pref => pref == "testPref"));
      Assert.ok(Preferences.get("testPref") === expectedValue);
      this.document.querySelector("#prefs button[data-l10n-id='about-config-pref-delete']").click();
    }
  });
});

add_task(async function test_delete_user_pref() {
  Services.prefs.setBoolPref("userAddedPref", true);
  await AboutConfigTest.withNewTab(async function() {
    let row = this.getRow("userAddedPref");
    row.resetColumnButton.click();
    Assert.ok(!this.getRow("userAddedPref"));
    Assert.ok(!Services.prefs.getChildList("").includes("userAddedPref"));

    // Search for nothing to test gPrefArray
    this.search();
    Assert.ok(!this.getRow("userAddedPref"));
  });
});

add_task(async function test_reset_user_pref() {
  await SpecialPowers.pushPrefEnv({
    "set": [
      [PREF_BOOLEAN_DEFAULT_TRUE, false],
      [PREF_STRING_LOCALIZED_MISSING, "user-value"],
    ],
  });

  await AboutConfigTest.withNewTab(async function() {
    // Click reset.
    let row = this.getRow(PREF_BOOLEAN_DEFAULT_TRUE);
    row.resetColumnButton.click();
    // Check new layout and reset.
    Assert.ok(!row.hasClass("has-user-value"));
    Assert.ok(!row.resetColumnButton);
    Assert.ok(!Services.prefs.prefHasUserValue(PREF_BOOLEAN_DEFAULT_TRUE));
    Assert.equal(this.getRow(PREF_BOOLEAN_DEFAULT_TRUE).value, "true");

    // Search for nothing to test gPrefArray
    this.search();
    row = this.getRow(PREF_BOOLEAN_DEFAULT_TRUE);
    Assert.ok(!row.hasClass("has-user-value"));
    Assert.ok(!row.resetColumnButton);
    Assert.equal(this.getRow(PREF_BOOLEAN_DEFAULT_TRUE).value, "true");

    // Clicking reset on a localized preference without a corresponding value.
    row = this.getRow(PREF_STRING_LOCALIZED_MISSING);
    Assert.equal(row.value, "user-value");
    row.resetColumnButton.click();
    // Check new layout and reset.
    Assert.ok(!row.hasClass("has-user-value"));
    Assert.ok(!row.resetColumnButton);
    Assert.ok(!Services.prefs.prefHasUserValue(PREF_STRING_LOCALIZED_MISSING));
    Assert.equal(this.getRow(PREF_STRING_LOCALIZED_MISSING).value, "");
  });
});

add_task(async function test_modify() {
  await AboutConfigTest.withNewTab(async function() {
    // Test toggle for boolean prefs.
    for (let nameOfBoolPref of [
      "test.aboutconfig.modify.boolean",
      PREF_BOOLEAN_DEFAULT_TRUE,
    ]) {
      let row = this.getRow(nameOfBoolPref);
      // Do this a two times to reset the pref.
      for (let i = 0; i < 2; i++) {
        row.editColumnButton.click();
        // Check new layout and saving in backend.
        Assert.equal(this.getRow(nameOfBoolPref).value,
          "" + Preferences.get(nameOfBoolPref));
        let prefHasUserValue = Services.prefs.prefHasUserValue(nameOfBoolPref);
        Assert.equal(row.hasClass("has-user-value"), prefHasUserValue);
        Assert.equal(!!row.resetColumnButton, prefHasUserValue);
      }
    }

    // Test abort of edit by starting with string and continuing with editing Int pref.
    let row = this.getRow("test.aboutconfig.modify.string");
    row.editColumnButton.click();
    row.valueInput.value = "test";
    let intRow = this.getRow("test.aboutconfig.modify.number");
    intRow.editColumnButton.click();
    Assert.equal(intRow.valueInput.value,
      Preferences.get("test.aboutconfig.modify.number"));
    Assert.ok(!row.valueInput);
    Assert.equal(row.value, Preferences.get("test.aboutconfig.modify.string"));

    // Test validation of integer values.
    for (let invalidValue of
         ["", " ", "a", "1.5", "-2147483649", "2147483648"]) {
      intRow.valueInput.value = invalidValue;
      intRow.editColumnButton.click();
      // We should still be in edit mode.
      Assert.ok(intRow.valueInput);
    }

    // Test correct saving and DOM-update.
    for (let [prefName, willDelete] of [
      ["test.aboutconfig.modify.string", true],
      ["test.aboutconfig.modify.number", true],
      [PREF_NUMBER_DEFAULT_ZERO, false],
      [PREF_STRING_DEFAULT_EMPTY, false],
    ]) {
      row = this.getRow(prefName);
      // Activate edit and check displaying.
      row.editColumnButton.click();
      Assert.equal(row.valueInput.value, Preferences.get(prefName));
      row.valueInput.value = "42";
      // Save and check saving.
      row.editColumnButton.click();
      Assert.equal(Preferences.get(prefName), "42");
      Assert.equal(row.value, "42");
      Assert.ok(row.hasClass("has-user-value"));
      // Reset or delete the preference while editing.
      row.editColumnButton.click();
      Assert.equal(row.valueInput.value, Preferences.get(prefName));
      row.resetColumnButton.click();
      if (willDelete) {
        Assert.ok(!this.getRow(prefName));
      } else {
        Assert.ok(!row.hasClass("has-user-value"));
      }
    }
  });
});
