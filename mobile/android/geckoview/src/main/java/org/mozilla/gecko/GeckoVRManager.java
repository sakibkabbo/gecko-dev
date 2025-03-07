/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * vim: ts=4 sw=4 expandtab:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko;

import org.mozilla.gecko.annotation.WrapForJNI;
import org.mozilla.gecko.util.ThreadUtils;

public class GeckoVRManager {
    private static long mExternalContext;

    @WrapForJNI
    public static synchronized long getExternalContext() {
      return mExternalContext;
    }

    public static synchronized void setExternalContext(final long externalContext) {
        mExternalContext = externalContext;
    }

}
