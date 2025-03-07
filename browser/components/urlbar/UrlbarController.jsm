/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = ["QueryContext", "UrlbarController"];

ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
XPCOMUtils.defineLazyModuleGetters(this, {
  AppConstants: "resource://gre/modules/AppConstants.jsm",
  // BrowserUsageTelemetry: "resource:///modules/BrowserUsageTelemetry.jsm",
  UrlbarPrefs: "resource:///modules/UrlbarPrefs.jsm",
  UrlbarProvidersManager: "resource:///modules/UrlbarProvidersManager.jsm",
});

const TELEMETRY_1ST_RESULT = "PLACES_AUTOCOMPLETE_1ST_RESULT_TIME_MS";
const TELEMETRY_6_FIRST_RESULTS = "PLACES_AUTOCOMPLETE_6_FIRST_RESULTS_TIME_MS";

/**
 * The address bar controller handles queries from the address bar, obtains
 * results and returns them to the UI for display.
 *
 * Listeners may be added to listen for the results. They must support the
 * following methods which may be called when a query is run:
 *
 * - onQueryStarted(queryContext)
 * - onQueryResults(queryContext)
 * - onQueryCancelled(queryContext)
 * - onQueryFinished(queryContext)
 */
class UrlbarController {
  /**
   * Initialises the class. The manager may be overridden here, this is for
   * test purposes.
   *
   * @param {object} options
   *   The initial options for UrlbarController.
   * @param {object} options.browserWindow
   *   The browser window this controller is operating within.
   * @param {object} [options.manager]
   *   Optional fake providers manager to override the built-in providers manager.
   *   Intended for use in unit tests only.
   */
  constructor(options = {}) {
    if (!options.browserWindow) {
      throw new Error("Missing options: browserWindow");
    }
    if (!options.browserWindow.location ||
        options.browserWindow.location.href != AppConstants.BROWSER_CHROME_URL) {
      throw new Error("browserWindow should be an actual browser window.");
    }

    this.manager = options.manager || UrlbarProvidersManager;
    this.browserWindow = options.browserWindow;

    this._listeners = new Set();
  }

  /**
   * Hooks up the controller with an input.
   *
   * @param {UrlbarInput} input
   *   The UrlbarInput instance associated with this controller.
   */
  setInput(input) {
    this.input = input;
  }

  /**
   * Hooks up the controller with a view.
   *
   * @param {UrlbarView} view
   *   The UrlbarView instance associated with this controller.
   */
  setView(view) {
    this.view = view;
  }

  /**
   * Takes a query context and starts the query based on the user input.
   *
   * @param {QueryContext} queryContext The query details.
   */
  async startQuery(queryContext) {
    // Cancel any running query.
    if (this._lastQueryContext) {
      this.cancelQuery(this._lastQueryContext);
    }
    this._lastQueryContext = queryContext;

    queryContext.autoFill = UrlbarPrefs.get("autoFill");

    queryContext.lastTelemetryResultCount = 0;
    TelemetryStopwatch.start(TELEMETRY_1ST_RESULT, queryContext);
    TelemetryStopwatch.start(TELEMETRY_6_FIRST_RESULTS, queryContext);

    this._notify("onQueryStarted", queryContext);
    await this.manager.startQuery(queryContext, this);
    this._notify("onQueryFinished", queryContext);
  }

  /**
   * Cancels an in-progress query. Note, queries may continue running if they
   * can't be canceled.
   *
   * @param {QueryContext} queryContext The query details.
   */
  cancelQuery(queryContext) {
    if (queryContext === this._lastQueryContext) {
      delete this._lastQueryContext;
    }

    TelemetryStopwatch.cancel(TELEMETRY_1ST_RESULT, queryContext);
    TelemetryStopwatch.cancel(TELEMETRY_6_FIRST_RESULTS, queryContext);

    this.manager.cancelQuery(queryContext);
    this._notify("onQueryCancelled", queryContext);
  }

  /**
   * Receives results from a query.
   *
   * @param {QueryContext} queryContext The query details.
   */
  receiveResults(queryContext) {
    if (queryContext.lastTelemetryResultCount < 1 &&
        queryContext.results.length >= 1) {
      TelemetryStopwatch.finish(TELEMETRY_1ST_RESULT, queryContext);
    }
    if (queryContext.lastTelemetryResultCount < 6 &&
        queryContext.results.length >= 6) {
      TelemetryStopwatch.finish(TELEMETRY_6_FIRST_RESULTS, queryContext);
    }
    queryContext.lastTelemetryResultCount = queryContext.results.length;

    this._notify("onQueryResults", queryContext);
  }

  /**
   * Adds a listener for query actions and results.
   *
   * @param {object} listener The listener to add.
   * @throws {TypeError} Throws if the listener is not an object.
   */
  addQueryListener(listener) {
    if (!listener || typeof listener != "object") {
      throw new TypeError("Expected listener to be an object");
    }
    this._listeners.add(listener);
  }

  /**
   * Removes a query listener.
   *
   * @param {object} listener The listener to add.
   */
  removeQueryListener(listener) {
    this._listeners.delete(listener);
  }

  /**
   * When switching tabs, clear some internal caches to handle cases like
   * backspace, autofill or repeated searches.
   */
  tabContextChanged() {
    // TODO: implementation needed (bug 1496685)
  }

  /**
   * Receives keyboard events from the input and handles those that should
   * navigate within the view or pick the currently selected item.
   *
   * @param {KeyboardEvent} event
   *   The DOM KeyboardEvent.
   */
  handleKeyNavigation(event) {
    // Handle readline/emacs-style navigation bindings on Mac.
    if (AppConstants.platform == "macosx" &&
        this.view.isOpen &&
        event.ctrlKey &&
        (event.key == "n" || event.key == "p")) {
      this.view.selectNextItem({ reverse: event.key == "p" });
      event.preventDefault();
      return;
    }

    switch (event.keyCode) {
      case KeyEvent.DOM_VK_ESCAPE:
        this.input.handleRevert();
        event.preventDefault();
        break;
      case KeyEvent.DOM_VK_RETURN:
        if (AppConstants.platform == "macosx" &&
            event.metaKey) {
          // Prevent beep on Mac.
          event.preventDefault();
        }
        // TODO: We may have an autoFill entry, so we should use that instead.
        // TODO: We should have an input bufferrer so that we can use search results
        // if appropriate.
        this.input.handleCommand(event);
        break;
      case KeyEvent.DOM_VK_TAB:
        if (this.view.isOpen) {
          this.view.selectNextItem({ reverse: event.shiftKey });
          event.preventDefault();
        }
        break;
      case KeyEvent.DOM_VK_DOWN:
      case KeyEvent.DOM_VK_UP:
        if (!event.ctrlKey && !event.altKey) {
          if (this.view.isOpen) {
            this.view.selectNextItem({
              reverse: event.keyCode == KeyEvent.DOM_VK_UP });
          } else {
            this.input.startQuery();
          }
          event.preventDefault();
        }
        break;
    }
  }

  /**
   * Internal function to notify listeners of results.
   *
   * @param {string} name Name of the notification.
   * @param {object} params Parameters to pass with the notification.
   */
  _notify(name, ...params) {
    for (let listener of this._listeners) {
      try {
        listener[name](...params);
      } catch (ex) {
        Cu.reportError(ex);
      }
    }
  }
}
