/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {
  CONNECT_RUNTIME_SUCCESS,
  DISCONNECT_RUNTIME_SUCCESS,
  RUNTIMES,
  UNWATCH_RUNTIME_SUCCESS,
  UPDATE_CONNECTION_PROMPT_SETTING_SUCCESS,
  UPDATE_RUNTIME_MULTIE10S_SUCCESS,
  REMOTE_RUNTIMES_UPDATED,
  WATCH_RUNTIME_SUCCESS,
} = require("../constants");

const {
  findRuntimeById,
} = require("../modules/runtimes-state-helper");

const { remoteClientManager } =
  require("devtools/client/shared/remote-debugging/remote-client-manager");

// Map between known runtime types and nodes in the runtimes state.
const TYPE_TO_RUNTIMES_KEY = {
  [RUNTIMES.THIS_FIREFOX]: "thisFirefoxRuntimes",
  [RUNTIMES.NETWORK]: "networkRuntimes",
  [RUNTIMES.USB]: "usbRuntimes",
};

function RuntimesState() {
  return {
    networkRuntimes: [],
    selectedRuntimeId: null,
    thisFirefoxRuntimes: [{
      id: RUNTIMES.THIS_FIREFOX,
      isUnknown: false,
      name: "This Firefox",
      type: RUNTIMES.THIS_FIREFOX,
    }],
    usbRuntimes: [],
  };
}

/**
 * Update the runtime matching the provided runtimeId with the content of updatedRuntime,
 * and return the new state.
 *
 * @param  {String} runtimeId
 *         The id of the runtime to update
 * @param  {Object} updatedRuntime
 *         Object used to update the runtime matching the idea using Object.assign.
 * @param  {Object} state
 *         Current runtimes state.
 * @return {Object} The updated state
 */
function _updateRuntimeById(runtimeId, updatedRuntime, state) {
  // Find the array of runtimes that contains the updated runtime.
  const runtime = findRuntimeById(runtimeId, state);
  const key = TYPE_TO_RUNTIMES_KEY[runtime.type];
  const runtimesToUpdate = state[key];

  // Update the runtime with the provided updatedRuntime.
  const updatedRuntimes = runtimesToUpdate.map(r => {
    if (r.id === runtimeId) {
      return Object.assign({}, r, updatedRuntime);
    }
    return r;
  });
  return Object.assign({}, state, { [key]: updatedRuntimes });
}

function runtimesReducer(state = RuntimesState(), action) {
  switch (action.type) {
    case CONNECT_RUNTIME_SUCCESS: {
      const { id, runtimeDetails, type } = action.runtime;
      remoteClientManager.setClient(id, type, runtimeDetails.clientWrapper.client);
      return _updateRuntimeById(id, { runtimeDetails }, state);
    }

    case DISCONNECT_RUNTIME_SUCCESS: {
      const { id, type } = action.runtime;
      remoteClientManager.removeClient(id, type);
      return _updateRuntimeById(id, { runtimeDetails: null }, state);
    }

    case UNWATCH_RUNTIME_SUCCESS: {
      return Object.assign({}, state, { selectedRuntimeId: null });
    }

    case UPDATE_CONNECTION_PROMPT_SETTING_SUCCESS: {
      const { connectionPromptEnabled } = action;
      const { id: runtimeId } = action.runtime;
      const runtime = findRuntimeById(runtimeId, state);
      const runtimeDetails =
        Object.assign({}, runtime.runtimeDetails, { connectionPromptEnabled });
      return _updateRuntimeById(runtimeId, { runtimeDetails }, state);
    }

    case UPDATE_RUNTIME_MULTIE10S_SUCCESS: {
      const { isMultiE10s } = action;
      const { id: runtimeId } = action.runtime;
      const runtime = findRuntimeById(runtimeId, state);
      const runtimeDetails =
        Object.assign({}, runtime.runtimeDetails, { isMultiE10s });
      return _updateRuntimeById(runtimeId, { runtimeDetails }, state);
    }

    case REMOTE_RUNTIMES_UPDATED: {
      const { runtimes, runtimeType } = action;
      const key = TYPE_TO_RUNTIMES_KEY[runtimeType];
      return Object.assign({}, state, {
        [key]: runtimes,
      });
    }

    case WATCH_RUNTIME_SUCCESS: {
      const { id } = action.runtime;
      return Object.assign({}, state, { selectedRuntimeId: id });
    }

    default:
      return state;
  }
}

module.exports = {
  RuntimesState,
  runtimesReducer,
};
