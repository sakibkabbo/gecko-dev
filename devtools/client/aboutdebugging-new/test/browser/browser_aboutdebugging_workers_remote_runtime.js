/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/* import-globals-from helper-mocks.js */
Services.scriptloader.loadSubScript(CHROME_URL_ROOT + "helper-mocks.js", this);

const NETWORK_RUNTIME_HOST = "localhost:6080";
const NETWORK_RUNTIME_APP_NAME = "TestNetworkApp";

const TESTS = [
  {
    category: "Other Workers",
    propertyName: "otherWorkers",
    workerName: "other/worker/script.js",
  }, {
    category: "Service Workers",
    propertyName: "serviceWorkers",
    workerName: "service/worker/script.js",
  }, {
    category: "Shared Workers",
    propertyName: "sharedWorkers",
    workerName: "shared/worker/script.js",
  },
];

const EMPTY_WORKERS_RESPONSE = {
  otherWorkers: [],
  serviceWorkers: [],
  sharedWorkers: [],
};

// Test that workers are displayed and updated for remote runtimes when expected.
add_task(async function() {
  const mocks = new Mocks();

  const { document, tab } = await openAboutDebugging();

  info("Prepare Network client mock");
  const networkClient = mocks.createNetworkRuntime(NETWORK_RUNTIME_HOST, {
    name: NETWORK_RUNTIME_APP_NAME,
  });

  info("Test workers in runtime page for Network client");
  await connectToRuntime(NETWORK_RUNTIME_HOST, document);
  await selectRuntime(NETWORK_RUNTIME_HOST, NETWORK_RUNTIME_APP_NAME, document);

  for (const testData of TESTS) {
    await testWorkerOnMockedRemoteClient(testData, networkClient, mocks.thisFirefoxClient,
     document);
  }

  await removeTab(tab);
});

/**
 * Check that workers are visible in the runtime page for a remote client.
 */
async function testWorkerOnMockedRemoteClient(testData, remoteClient, firefoxClient,
  document) {
  const { category, propertyName, workerName } = testData;
  info(`Test workers for category [${category}] in remote runtime`);

  const workersPane = getDebugTargetPane(category, document);
  info("Check an empty target pane message is displayed");
  ok(workersPane.querySelector(".js-debug-target-list-empty"),
    "Workers list is empty");

  info(`Add a worker of type [${propertyName}] to the remote client`);
  const workers = Object.assign({}, EMPTY_WORKERS_RESPONSE, {
    [propertyName]: [{
      name: workerName,
      workerTargetFront: {
        actorID: workerName,
      },
    }],
  });
  remoteClient.listWorkers = () => workers;
  remoteClient._eventEmitter.emit("workerListChanged");

  info("Wait until the worker appears");
  await waitUntil(() => !workersPane.querySelector(".js-debug-target-list-empty"));

  const workerTarget = findDebugTargetByText(workerName, document);
  ok(workerTarget, "Worker target appeared for the remote runtime");

  // Check that the list of REMOTE workers are NOT updated when the local this-firefox
  // emits a workerListChanged event.
  info("Remove the worker from the remote client WITHOUT sending an event");
  remoteClient.listWorkers = () => EMPTY_WORKERS_RESPONSE;

  info("Simulate a worker update on the ThisFirefox client");
  firefoxClient._eventEmitter.emit("workerListChanged");

  // To avoid wait for a set period of time we trigger another async update, adding a new
  // tab. We assume that if the worker update mechanism had started, it would also be done
  // when the new tab was processed.
  info("Wait until the tab target for 'http://some.random/url.com' appears");
  const testTab = { outerWindowID: 0, url: "http://some.random/url.com" };
  remoteClient.listTabs = () => [testTab];
  remoteClient._eventEmitter.emit("tabListChanged");
  await waitUntil(() => findDebugTargetByText("http://some.random/url.com", document));

  ok(findDebugTargetByText(workerName, document),
    "The test worker is still visible");

  info("Emit `workerListChanged` on remoteClient and wait for the target list to update");
  remoteClient._eventEmitter.emit("workerListChanged");
  await waitUntil(() => !findDebugTargetByText(workerName, document));
}
