/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const {workerTargetSpec} = require("devtools/shared/specs/targets/worker");
const { FrontClassWithSpec, registerFront } = require("devtools/shared/protocol");

loader.lazyRequireGetter(this, "ThreadClient", "devtools/shared/client/thread-client");

class WorkerTargetFront extends FrontClassWithSpec(workerTargetSpec) {
  constructor(client, form) {
    super(client, form);

    this.thread = null;
    this.traits = {};

    // TODO: remove once ThreadClient becomes a front
    this.client = client;

    this._isClosed = false;

    this.destroy = this.destroy.bind(this);
    this.on("close", this.destroy);
  }

  form(json) {
    this.actorID = json.actor;

    // Save the full form for Target class usage.
    // Do not use `form` name to avoid colliding with protocol.js's `form` method
    this.targetForm = json;
    this.url = json.url;
    this.type = json.type;
    this.scope = json.scope;
    this.fetch = json.fetch;
  }

  get isClosed() {
    return this._isClosed;
  }

  destroy() {
    this.off("close", this.destroy);
    this._isClosed = true;

    if (this.thread) {
      this.client.unregisterClient(this.thread);
    }

    this.unmanage(this);

    super.destroy();
  }

  async attach() {
    const response = await super.attach();

    this.url = response.url;

    // Immediately call `connect` in other to fetch console and thread actors
    // that will be later used by Target.
    const connectResponse = await this.connect({});
    // Set the console actor ID on the form to expose it to Target.attach's attachConsole
    this.targetForm.consoleActor = connectResponse.consoleActor;
    this.threadActor = connectResponse.threadActor;

    return response;
  }

  async detach() {
    if (this.isClosed) {
      return {};
    }
    let response;
    try {
      response = await super.detach();
    } catch (e) {
      console.warn(`Error while detaching the worker target front: ${e.message}`);
    }
    this.destroy();
    return response;
  }

  reconfigure() {
    // Toolbox and options panel are calling this method but Worker Target can't be
    // reconfigured. So we ignore this call here.
    return Promise.resolve();
  }

  async attachThread(options = {}) {
    if (this.thread) {
      const response = [{
        type: "connected",
        threadActor: this.thread._actor,
        consoleActor: this.targetForm.consoleActor,
      }, this.thread];
      return response;
    }

    const attachResponse = await this.client.request({
      to: this.threadActor,
      type: "attach",
      options,
    });
    this.thread = new ThreadClient(this, this.threadActor);
    this.client.registerClient(this.thread);

    return [attachResponse, this.thread];
  }
}

exports.WorkerTargetFront = WorkerTargetFront;
registerFront(WorkerTargetFront);
