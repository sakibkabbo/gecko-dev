/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

ChromeUtils.import("resource://testing-common/httpd.js");
ChromeUtils.import("resource://gre/modules/NetUtil.jsm");
ChromeUtils.import("resource://gre/modules/Services.jsm");

const nsIDocumentEncoder = Ci.nsIDocumentEncoder;
const replacementChar = Ci.nsIConverterInputStream.DEFAULT_REPLACEMENT_CHARACTER;

function loadContentFile(aFile, aCharset) {
    // if(aAsIso == undefined) aAsIso = false;
    if (aCharset == undefined)
        aCharset = "UTF-8";

    var file = do_get_file(aFile);

    var chann = NetUtil.newChannel({
      uri: Services.io.newFileURI(file),
      loadUsingSystemPrincipal: true,
    });
    chann.contentCharset = aCharset;

    /* var inputStream = Components.classes["@mozilla.org/scriptableinputstream;1"]
                        .createInstance(Components.interfaces.nsIScriptableInputStream);
    inputStream.init(chann.open2());
    return inputStream.read(file.fileSize);
    */

    var inputStream = Cc["@mozilla.org/intl/converter-input-stream;1"]
                       .createInstance(Ci.nsIConverterInputStream);
    inputStream.init(chann.open2(), aCharset, 1024, replacementChar);
    var str = {}, content = "";
    while (inputStream.readString(4096, str) != 0) {
        content += str.value;
    }
    return content;
}
