/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { createFactory, PureComponent } = require("devtools/client/shared/vendor/react");
const PropTypes = require("devtools/client/shared/vendor/react-prop-types");

const Rule = createFactory(require("./Rule"));

const Types = require("../types");

class Rules extends PureComponent {
  static get propTypes() {
    return {
      rules: PropTypes.arrayOf(PropTypes.shape(Types.rule)).isRequired,
    };
  }

  render() {
    return this.props.rules.map(rule => {
      return Rule({
        key: rule.id,
        rule,
      });
    });
  }
}

module.exports = Rules;
