/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Actions = require("./index");

const {
  getAllRuntimes,
  getCurrentRuntime,
  findRuntimeById,
} = require("../modules/runtimes-state-helper");
const { isSupportedDebugTarget } = require("../modules/debug-target-support");

const { createClientForRuntime } = require("../modules/runtime-client-factory");

const { remoteClientManager } =
  require("devtools/client/shared/remote-debugging/remote-client-manager");

const {
  CONNECT_RUNTIME_FAILURE,
  CONNECT_RUNTIME_START,
  CONNECT_RUNTIME_SUCCESS,
  DEBUG_TARGETS,
  DISCONNECT_RUNTIME_FAILURE,
  DISCONNECT_RUNTIME_START,
  DISCONNECT_RUNTIME_SUCCESS,
  PAGE_TYPES,
  REMOTE_RUNTIMES_UPDATED,
  RUNTIME_PREFERENCE,
  RUNTIMES,
  UNWATCH_RUNTIME_FAILURE,
  UNWATCH_RUNTIME_START,
  UNWATCH_RUNTIME_SUCCESS,
  UPDATE_CONNECTION_PROMPT_SETTING_FAILURE,
  UPDATE_CONNECTION_PROMPT_SETTING_START,
  UPDATE_CONNECTION_PROMPT_SETTING_SUCCESS,
  UPDATE_RUNTIME_MULTIE10S_FAILURE,
  UPDATE_RUNTIME_MULTIE10S_START,
  UPDATE_RUNTIME_MULTIE10S_SUCCESS,
  WATCH_RUNTIME_FAILURE,
  WATCH_RUNTIME_START,
  WATCH_RUNTIME_SUCCESS,
} = require("../constants");

async function getRuntimeInfo(runtime, clientWrapper) {
  const { type } = runtime;
  const { name, channel, deviceName, isMultiE10s, version } =
    await clientWrapper.getDeviceDescription();
  const icon =
    (channel === "release" || channel === "beta" || channel === "aurora")
      ? `chrome://devtools/skin/images/aboutdebugging-firefox-${ channel }.svg`
      : "chrome://devtools/skin/images/aboutdebugging-firefox-nightly.svg";

  return {
    deviceName,
    icon,
    isMultiE10s,
    name,
    type,
    version,
  };
}

function onRemoteDebuggerClientClosed() {
  window.AboutDebugging.onNetworkLocationsUpdated();
  window.AboutDebugging.onUSBRuntimesUpdated();
}

function onMultiE10sUpdated() {
  window.AboutDebugging.store.dispatch(updateMultiE10s());
}

function connectRuntime(id) {
  return async (dispatch, getState) => {
    dispatch({ type: CONNECT_RUNTIME_START });
    try {
      const runtime = findRuntimeById(id, getState().runtimes);
      const clientWrapper = await createClientForRuntime(runtime);
      const info = await getRuntimeInfo(runtime, clientWrapper);
      const { isMultiE10s } = info;
      delete info.isMultiE10s;

      const promptPrefName = RUNTIME_PREFERENCE.CONNECTION_PROMPT;
      const connectionPromptEnabled = await clientWrapper.getPreference(promptPrefName);
      const runtimeDetails = {
        clientWrapper,
        connectionPromptEnabled,
        info,
        isMultiE10s,
      };

      const deviceFront = await clientWrapper.getFront("device");
      if (deviceFront) {
        deviceFront.on("multi-e10s-updated", onMultiE10sUpdated);
      }

      if (runtime.type !== RUNTIMES.THIS_FIREFOX) {
        // `closed` event will be emitted when disabling remote debugging
        // on the connected remote runtime.
        clientWrapper.addOneTimeListener("closed", onRemoteDebuggerClientClosed);
      }

      dispatch({
        type: CONNECT_RUNTIME_SUCCESS,
        runtime: {
          id,
          runtimeDetails,
          type: runtime.type,
        },
      });
    } catch (e) {
      dispatch({ type: CONNECT_RUNTIME_FAILURE, error: e });
    }
  };
}

function disconnectRuntime(id) {
  return async (dispatch, getState) => {
    dispatch({ type: DISCONNECT_RUNTIME_START });
    try {
      const runtime = findRuntimeById(id, getState().runtimes);
      const { clientWrapper } = runtime.runtimeDetails;

      const deviceFront = await clientWrapper.getFront("device");
      if (deviceFront) {
        deviceFront.off("multi-e10s-updated", onMultiE10sUpdated);
      }

      if (runtime.type !== RUNTIMES.THIS_FIREFOX) {
        clientWrapper.removeListener("closed", onRemoteDebuggerClientClosed);
      }

      await clientWrapper.close();

      dispatch({
        type: DISCONNECT_RUNTIME_SUCCESS,
        runtime: {
          id,
          type: runtime.type,
        },
      });
    } catch (e) {
      dispatch({ type: DISCONNECT_RUNTIME_FAILURE, error: e });
    }
  };
}

function updateConnectionPromptSetting(connectionPromptEnabled) {
  return async (dispatch, getState) => {
    dispatch({ type: UPDATE_CONNECTION_PROMPT_SETTING_START });
    try {
      const runtime = getCurrentRuntime(getState().runtimes);
      const { clientWrapper } = runtime.runtimeDetails;
      const promptPrefName = RUNTIME_PREFERENCE.CONNECTION_PROMPT;
      await clientWrapper.setPreference(promptPrefName, connectionPromptEnabled);
      // Re-get actual value from the runtime.
      connectionPromptEnabled = await clientWrapper.getPreference(promptPrefName);

      dispatch({ type: UPDATE_CONNECTION_PROMPT_SETTING_SUCCESS,
                 runtime, connectionPromptEnabled });
    } catch (e) {
      dispatch({ type: UPDATE_CONNECTION_PROMPT_SETTING_FAILURE, error: e });
    }
  };
}

function updateMultiE10s() {
  return async (dispatch, getState) => {
    dispatch({ type: UPDATE_RUNTIME_MULTIE10S_START });
    try {
      const runtime = getCurrentRuntime(getState().runtimes);
      const { clientWrapper } = runtime.runtimeDetails;
      // Re-get actual value from the runtime.
      const { isMultiE10s } = await clientWrapper.getDeviceDescription();

      dispatch({ type: UPDATE_RUNTIME_MULTIE10S_SUCCESS, runtime, isMultiE10s });
    } catch (e) {
      dispatch({ type: UPDATE_RUNTIME_MULTIE10S_FAILURE, error: e });
    }
  };
}

function watchRuntime(id) {
  return async (dispatch, getState) => {
    dispatch({ type: WATCH_RUNTIME_START });

    try {
      if (id === RUNTIMES.THIS_FIREFOX) {
        // THIS_FIREFOX connects and disconnects on the fly when opening the page.
        await dispatch(connectRuntime(RUNTIMES.THIS_FIREFOX));
      }

      // The selected runtime should already have a connected client assigned.
      const runtime = findRuntimeById(id, getState().runtimes);
      await dispatch({ type: WATCH_RUNTIME_SUCCESS, runtime });

      if (isSupportedDebugTarget(runtime.type, DEBUG_TARGETS.EXTENSION)) {
        dispatch(Actions.requestExtensions());
      }

      if (isSupportedDebugTarget(runtime.type, DEBUG_TARGETS.TAB)) {
        dispatch(Actions.requestTabs());
      }

      if (isSupportedDebugTarget(runtime.type, DEBUG_TARGETS.WORKER)) {
        dispatch(Actions.requestWorkers());
      }
    } catch (e) {
      dispatch({ type: WATCH_RUNTIME_FAILURE, error: e });
    }
  };
}

function unwatchRuntime(id) {
  return async (dispatch, getState) => {
    const runtime = findRuntimeById(id, getState().runtimes);

    dispatch({ type: UNWATCH_RUNTIME_START, runtime });

    try {
      if (id === RUNTIMES.THIS_FIREFOX) {
        // THIS_FIREFOX connects and disconnects on the fly when opening the page.
        await dispatch(disconnectRuntime(RUNTIMES.THIS_FIREFOX));
      }

      dispatch({ type: UNWATCH_RUNTIME_SUCCESS });
    } catch (e) {
      dispatch({ type: UNWATCH_RUNTIME_FAILURE, error: e });
    }
  };
}

function updateNetworkRuntimes(locations) {
  const runtimes = locations.map(location => {
    const [ host, port ] = location.split(":");
    return {
      id: location,
      extra: {
        connectionParameters: { host, port: parseInt(port, 10) },
      },
      isUnknown: false,
      name: location,
      type: RUNTIMES.NETWORK,
    };
  });
  return updateRemoteRuntimes(runtimes, RUNTIMES.NETWORK);
}

function updateUSBRuntimes(adbRuntimes) {
  const runtimes = adbRuntimes.map(adbRuntime => {
    // Set connectionParameters only for known runtimes.
    const socketPath = adbRuntime._socketPath;
    const deviceId = adbRuntime.deviceId;
    const connectionParameters = adbRuntime.isUnknown() ? null : { deviceId, socketPath };
    return {
      id: adbRuntime.id,
      extra: {
        connectionParameters,
        deviceName: adbRuntime.deviceName,
      },
      isUnknown: adbRuntime.isUnknown(),
      name: adbRuntime.shortName,
      type: RUNTIMES.USB,
    };
  });
  return updateRemoteRuntimes(runtimes, RUNTIMES.USB);
}

/**
 * Check that a given runtime can still be found in the provided array of runtimes, and
 * that the connection of the associated DebuggerClient is still valid.
 * Note that this check is only valid for runtimes which match the type of the runtimes
 * in the array.
 */
function _isRuntimeValid(runtime, runtimes) {
  const isRuntimeAvailable = runtimes.some(r => r.id === runtime.id);
  const isConnectionValid = runtime.runtimeDetails &&
    !runtime.runtimeDetails.clientWrapper.isClosed();
  return isRuntimeAvailable && isConnectionValid;
}

function updateRemoteRuntimes(runtimes, type) {
  return async (dispatch, getState) => {
    const currentRuntime = getCurrentRuntime(getState().runtimes);

    // Check if the updated remote runtimes should trigger a navigation out of the current
    // runtime page.
    if (currentRuntime && currentRuntime.type === type &&
      !_isRuntimeValid(currentRuntime, runtimes)) {
      // Since current remote runtime is invalid, move to this firefox page.
      // This case is considered as followings and so on:
      // * Remove ADB addon
      // * (Physically) Disconnect USB runtime
      //
      // The reason we call selectPage before REMOTE_RUNTIMES_UPDATED is fired is below.
      // Current runtime can not be retrieved after REMOTE_RUNTIMES_UPDATED action, since
      // that updates runtime state. So, before that we fire selectPage action to execute
      // `unwatchRuntime` correctly.
      await dispatch(Actions.selectPage(PAGE_TYPES.RUNTIME, RUNTIMES.THIS_FIREFOX));
    }

    // Retrieve runtimeDetails from existing runtimes.
    runtimes.forEach(runtime => {
      const existingRuntime = findRuntimeById(runtime.id, getState().runtimes);
      const isConnectionValid = existingRuntime && existingRuntime.runtimeDetails &&
        !existingRuntime.runtimeDetails.clientWrapper.isClosed();
      runtime.runtimeDetails = isConnectionValid ? existingRuntime.runtimeDetails : null;
    });

    const existingRuntimes = getAllRuntimes(getState().runtimes);
    for (const runtime of existingRuntimes) {
      // Runtime was connected before.
      const isConnected = runtime.runtimeDetails;
      // Runtime is of the same type as the updated runtimes array, so we should check it.
      const isSameType = runtime.type === type;
      if (isConnected && isSameType && !_isRuntimeValid(runtime, runtimes)) {
        // Disconnect runtimes that were no longer valid.
        await dispatch(disconnectRuntime(runtime.id));
      }
    }

    dispatch({ type: REMOTE_RUNTIMES_UPDATED, runtimes, runtimeType: type });

    for (const runtime of getAllRuntimes(getState().runtimes)) {
      if (runtime.type !== type) {
        continue;
      }

      // Reconnect clients already available in the RemoteClientManager.
      const isConnected = !!runtime.runtimeDetails;
      const hasConnectedClient = remoteClientManager.hasClient(runtime.id, runtime.type);
      if (!isConnected && hasConnectedClient) {
        await dispatch(connectRuntime(runtime.id));
      }
    }
  };
}

/**
 * Remove all the listeners added on client objects. Since those objects are persisted
 * regardless of the about:debugging lifecycle, all the added events should be removed
 * before leaving about:debugging.
 */
function removeRuntimeListeners() {
  return (dispatch, getState) => {
    const allRuntimes = getAllRuntimes(getState().runtimes);
    const remoteRuntimes = allRuntimes.filter(r => r.type !== RUNTIMES.THIS_FIREFOX);
    for (const runtime of remoteRuntimes) {
      if (runtime.runtimeDetails) {
        const { clientWrapper } = runtime.runtimeDetails;
        clientWrapper.removeListener("closed", onRemoteDebuggerClientClosed);
      }
    }
  };
}

module.exports = {
  connectRuntime,
  disconnectRuntime,
  removeRuntimeListeners,
  unwatchRuntime,
  updateConnectionPromptSetting,
  updateNetworkRuntimes,
  updateUSBRuntimes,
  watchRuntime,
};
