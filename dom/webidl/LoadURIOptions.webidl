/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

interface Principal;
interface URI;
interface InputStream;

/**
 * This dictionary holds load arguments for docshell loads.
 */

dictionary LoadURIOptions {
  /**
   * The principal that initiated the load.
   */
  Principal? triggeringPrincipal = null;

  /**
   * Flags modifying load behaviour.  This parameter is a bitwise
   * combination of the load flags defined in nsIWebNavigation.idl.
   */
   long loadFlags = 0;

  /**
   * The referring URI.  If this argument is null, then the referring
   * URI will be inferred internally.
   */
  URI? referrerURI = null;

  /**
   * Referrer Policy for the load, defaults to REFERRER_POLICY_UNSET.
   * Alternatively use one of REFERRER_POLICY_* constants from
   * nsIHttpChannel.
   */
  long referrerPolicy = 0;

  /**
   * If the URI to be loaded corresponds to a HTTP request, then this stream is
   * appended directly to the HTTP request headers.  It may be prefixed
   * with additional HTTP headers.  This stream must contain a "\r\n"
   * sequence separating any HTTP headers from the HTTP request body.
   */
  InputStream? postData = null;

  /**
   * If the URI corresponds to a HTTP request, then any HTTP headers
   * contained in this stream are set on the HTTP request.  The HTTP
   * header stream is formatted as:
   *     ( HEADER "\r\n" )*
   */
   InputStream? headers = null;

  /**
   * Set to indicate a base URI to be associated with the load. Note
   * that at present this argument is only used with view-source aURIs
   * and cannot be used to resolve aURI.
   */
  URI? baseURI = null;
};