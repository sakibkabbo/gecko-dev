/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * These tests unit test the functionality of UrlbarController by stubbing out the
 * model and providing stubs to be called.
 */

"use strict";

const TEST_URL = "http://example.com";
const MATCH = new UrlbarMatch(UrlbarUtils.MATCH_TYPE.TAB_SWITCH,
                              UrlbarUtils.MATCH_SOURCE.TABS,
                              { url: TEST_URL });
const TELEMETRY_1ST_RESULT = "PLACES_AUTOCOMPLETE_1ST_RESULT_TIME_MS";
const TELEMETRY_6_FIRST_RESULTS = "PLACES_AUTOCOMPLETE_6_FIRST_RESULTS_TIME_MS";

let controller;
let firstHistogram;
let sixthHistogram;

/**
 * A delayed test provider, allowing the query to be delayed for an amount of time.
 */
class DelayedProvider extends UrlbarProvider {
  constructor() {
    super();
    this._name = "TestProvider" + Math.floor(Math.random() * 100000);
  }
  get name() {
    return this._name;
  }
  get type() {
    return UrlbarUtils.PROVIDER_TYPE.PROFILE;
  }
  get sources() {
    return [UrlbarUtils.MATCH_SOURCE.TABS];
  }
  async startQuery(context, add) {
    Assert.ok(context, "context is passed-in");
    Assert.equal(typeof add, "function", "add is a callback");
    this._context = context;
    this._add = add;
  }
  cancelQuery(context) {
    Assert.ok(false, "cancelQuery should not be called");
  }
  addResults(matches) {
    for (const match of matches) {
      this._add(this, match);
    }
  }
}

/**
 * Returns the number of reports sent recorded within the histogram results.
 *
 * @param {object} results a snapshot of histogram results to check.
 * @returns {number} The count of reports recorded in the histogram.
 */
function getHistogramReportsCount(results) {
  let sum = 0;
  for (let [, value] of Object.entries(results.values)) {
    sum += value;
  }
  return sum;
}

add_task(function setup() {
  controller = new UrlbarController({
    browserWindow: {
      location: {
        href: AppConstants.BROWSER_CHROME_URL,
      },
    },
  });

  firstHistogram = Services.telemetry.getHistogramById(TELEMETRY_1ST_RESULT);
  sixthHistogram = Services.telemetry.getHistogramById(TELEMETRY_6_FIRST_RESULTS);
});

add_task(async function test_n_autocomplete_cancel() {
  firstHistogram.clear();
  sixthHistogram.clear();

  let providerCanceledDeferred = PromiseUtils.defer();
  let provider = new TestProvider([], providerCanceledDeferred.resolve);
  UrlbarProvidersManager.registerProvider(provider);
  const context = createContext(TEST_URL, {providers: [provider.name]});

  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should not have started first result stopwatch");
  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should not have started first 6 results stopwatch");

  await controller.startQuery(context);

  Assert.ok(TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should have started first result stopwatch");
  Assert.ok(TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should have started first 6 results stopwatch");

  controller.cancelQuery(context);

  await providerCanceledDeferred.promise;

  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should have canceled first result stopwatch");
  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should have canceled first 6 results stopwatch");

  let results = firstHistogram.snapshot();
  Assert.equal(results.sum, 0, "Should not have recorded any times (first result)");
  results = sixthHistogram.snapshot();
  Assert.equal(results.sum, 0, "Should not have recorded any times (first 6 results)");
});

add_task(async function test_n_autocomplete_results() {
  firstHistogram.clear();
  sixthHistogram.clear();

  let provider = new DelayedProvider();
  UrlbarProvidersManager.registerProvider(provider);
  const context = createContext(TEST_URL, {providers: [provider.name]});

  let resultsPromise = promiseControllerNotification(controller, "onQueryResults");

  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should not have started first result stopwatch");
  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should not have started first 6 results stopwatch");

  await controller.startQuery(context);

  Assert.ok(TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should have started first result stopwatch");
  Assert.ok(TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should have started first 6 results stopwatch");

  provider.addResults([MATCH]);
  await resultsPromise;

  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should have stopped the first stopwatch");
  Assert.ok(TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should have kept the first 6 results stopwatch running");

  let firstResults = firstHistogram.snapshot();
  let first6Results = sixthHistogram.snapshot();
  Assert.equal(getHistogramReportsCount(firstResults), 1,
    "Should have recorded one time for the first result");
  Assert.equal(getHistogramReportsCount(first6Results), 0,
    "Should not have recorded any times (first 6 results)");

  // Now add 5 more results, so that the first 6 results is triggered.
  for (let i = 0; i < 5; i++) {
    resultsPromise = promiseControllerNotification(controller, "onQueryResults");
    provider.addResults([
      new UrlbarMatch(UrlbarUtils.MATCH_TYPE.TAB_SWITCH,
                      UrlbarUtils.MATCH_SOURCE.TABS,
                      { url: TEST_URL + "/i" }),
    ]);
    await resultsPromise;
  }

  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_1ST_RESULT, context),
    "Should have stopped the first stopwatch");
  Assert.ok(!TelemetryStopwatch.running(TELEMETRY_6_FIRST_RESULTS, context),
    "Should have stopped the first 6 results stopwatch");

  let updatedResults = firstHistogram.snapshot();
  let updated6Results = sixthHistogram.snapshot();
  Assert.deepEqual(updatedResults, firstResults,
    "Should not have changed the histogram for the first result");
  Assert.equal(getHistogramReportsCount(updated6Results), 1,
    "Should have recorded one time for the first 6 results");

  // Add one more, to check neither are updated.
  resultsPromise = promiseControllerNotification(controller, "onQueryResults");
  provider.addResults([
    new UrlbarMatch(UrlbarUtils.MATCH_TYPE.TAB_SWITCH,
                    UrlbarUtils.MATCH_SOURCE.TABS,
                    { url: TEST_URL + "/6" }),
  ]);
  await resultsPromise;

  let secondUpdateResults = firstHistogram.snapshot();
  let secondUpdate6Results = sixthHistogram.snapshot();
  Assert.deepEqual(secondUpdateResults, firstResults,
    "Should not have changed the histogram for the first result");
  Assert.equal(getHistogramReportsCount(secondUpdate6Results), 1,
    "Should not have changed the histogram for the first 6 results");
});
