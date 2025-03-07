/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// This head file contains helpers to create mock versions of the ClientWrapper class
// defined at devtools/client/aboutdebugging-new/src/modules/client-wrapper.js .

const { RUNTIME_PREFERENCE } =
  require("devtools/client/aboutdebugging-new/src/constants");

// Sensible default values for runtime preferences that should be usable in most
// situations
const DEFAULT_PREFERENCES = {
  [RUNTIME_PREFERENCE.CONNECTION_PROMPT]: true,
};

// Creates a simple mock ClientWrapper.
function createClientMock() {
  const EventEmitter = require("devtools/shared/event-emitter");
  const eventEmitter = {};
  EventEmitter.decorate(eventEmitter);

  return {
    // add a reference to the internal event emitter so that consumers can fire client
    // events.
    _eventEmitter: eventEmitter,
    _preferences: {},
    contentProcessFronts: [],
    serviceWorkerRegistrationFronts: [],
    addOneTimeListener: (evt, listener) => {
      eventEmitter.once(evt, listener);
    },
    addListener: (evt, listener) => {
      eventEmitter.on(evt, listener);
    },
    removeListener: (evt, listener) => {
      eventEmitter.off(evt, listener);
    },

    client: {
      addOneTimeListener: (evt, listener) => {
        eventEmitter.once(evt, listener);
      },
      addListener: (evt, listener) => {
        eventEmitter.on(evt, listener);
      },
      removeListener: (evt, listener) => {
        eventEmitter.off(evt, listener);
      },
    },
    // no-op
    close: () => {},
    // client is not closed
    isClosed: () => false,
    // no-op
    connect: () => {},
    // no-op
    getDeviceDescription: () => {},
    // Return default preference value or null if no match.
    getPreference: function(prefName) {
      if (prefName in this._preferences) {
        return this._preferences[prefName];
      }
      if (prefName in DEFAULT_PREFERENCES) {
        return DEFAULT_PREFERENCES[prefName];
      }
      return null;
    },
    // Empty array of addons
    listAddons: () => [],
    // Empty array of tabs
    listTabs: () => [],
    // Empty arrays of workers
    listWorkers: () => ({
      otherWorkers: [],
      serviceWorkers: [],
      sharedWorkers: [],
    }),
    // no-op
    getFront: () => {},
    // no-op
    onFront: () => {},
    // stores the preference locally (doesn't update about:config)
    setPreference: function(prefName, value) {
      this._preferences[prefName] = value;
    },
  };
}

// Create a ClientWrapper mock that can be used to replace the this-firefox runtime.
function createThisFirefoxClientMock() {
  const mockThisFirefoxDescription = {
    name: "Firefox",
    channel: "nightly",
    version: "63.0",
  };

  // Create a fake about:debugging tab because our test helper openAboutDebugging
  // waits until about:debugging is displayed in the list of tabs.
  const mockAboutDebuggingTab = {
    outerWindowID: 0,
    url: "about:debugging",
  };

  const mockThisFirefoxClient = createClientMock();
  mockThisFirefoxClient.listTabs = () => ([mockAboutDebuggingTab]);
  mockThisFirefoxClient.getDeviceDescription = () => mockThisFirefoxDescription;

  return mockThisFirefoxClient;
}
/* exported createThisFirefoxClientMock */

