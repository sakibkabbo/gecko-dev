/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");

ChromeUtils.defineModuleGetter(this, "LoginHelper",
                               "resource://gre/modules/LoginHelper.jsm");


function nsLoginInfo() {}

nsLoginInfo.prototype = {

  classID: Components.ID("{0f2f347c-1e4f-40cc-8efd-792dea70a85e}"),
  QueryInterface: ChromeUtils.generateQI([Ci.nsILoginInfo, Ci.nsILoginMetaInfo]),

  //
  // nsILoginInfo interfaces...
  //

  hostname: null,
  formSubmitURL: null,
  httpRealm: null,
  username: null,
  password: null,
  usernameField: null,
  passwordField: null,

  init(aHostname, aFormSubmitURL, aHttpRealm,
       aUsername, aPassword,
       aUsernameField, aPasswordField) {
    this.hostname      = aHostname;
    this.formSubmitURL = aFormSubmitURL;
    this.httpRealm     = aHttpRealm;
    this.username      = aUsername;
    this.password      = aPassword;
    this.usernameField = aUsernameField;
    this.passwordField = aPasswordField;
  },

  matches(aLogin, ignorePassword) {
    return LoginHelper.doLoginsMatch(this, aLogin, {
      ignorePassword,
    });
  },

  equals(aLogin) {
    if (this.hostname != aLogin.hostname ||
        this.formSubmitURL != aLogin.formSubmitURL ||
        this.httpRealm != aLogin.httpRealm ||
        this.username != aLogin.username ||
        this.password != aLogin.password ||
        this.usernameField != aLogin.usernameField ||
        this.passwordField != aLogin.passwordField) {
      return false;
    }

    return true;
  },

  clone() {
    let clone = Cc["@mozilla.org/login-manager/loginInfo;1"].
                createInstance(Ci.nsILoginInfo);
    clone.init(this.hostname, this.formSubmitURL, this.httpRealm,
               this.username, this.password,
               this.usernameField, this.passwordField);

    // Copy nsILoginMetaInfo props
    clone.QueryInterface(Ci.nsILoginMetaInfo);
    clone.guid = this.guid;
    clone.timeCreated = this.timeCreated;
    clone.timeLastUsed = this.timeLastUsed;
    clone.timePasswordChanged = this.timePasswordChanged;
    clone.timesUsed = this.timesUsed;

    return clone;
  },

  //
  // nsILoginMetaInfo interfaces...
  //

  guid: null,
  timeCreated: null,
  timeLastUsed: null,
  timePasswordChanged: null,
  timesUsed: null,

}; // end of nsLoginInfo implementation

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([nsLoginInfo]);
