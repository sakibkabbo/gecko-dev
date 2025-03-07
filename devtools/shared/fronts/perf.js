/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const { FrontClassWithSpec, registerFront } = require("devtools/shared/protocol");
const { perfSpec } = require("devtools/shared/specs/perf");

class PerfFront extends FrontClassWithSpec(perfSpec) {
  constructor(client, form) {
    super(client, { actor: form.perfActor });
    this.manage(this);
  }
}

registerFront(PerfFront);
