add_task(async function testBasicPrompt() {
  SpecialPowers.pushPrefEnv({set: [
    [PREF_APP_UPDATE_STAGING_ENABLED, true],
  ]});
  await UpdateUtils.setAppUpdateAutoEnabled(false);

  let updateParams = "promptWaitTime=0";
  gUseTestUpdater = true;

  await runUpdateTest(updateParams, 1, [
    {
      notificationId: "update-available",
      button: "button",
      beforeClick() {
        checkWhatsNewLink(window, "update-available-whats-new");
      },
    },
    {
      notificationId: "update-restart",
      button: "secondarybutton",
      cleanup() {
        AppMenuNotifications.removeNotification(/.*/);
      },
    },
  ]);
});
