<?php

/**
 * Extension of Better Getter class for handling HTTP connections.
 *
 * An aspect of handling HTTP which is unique is HTTP headers, which are sent
 * by both the client and the server as part of a connection. This class
 * features methods for specifying outgoing HTTP headers for a connection, as
 * well as parsing out incoming HTTP headers from the server response.
 *
 * @todo: Authentication, compression, HTTPS/SSL certificate handling, cookie
 * handling, et al.
 */
class BetterGetterHttp extends BetterGetter {

  /**
   * An associative array of headers to be sent as part of the server query,
   * keyed by header field name.
   *
   * Each value is a flat array of values. Why an array instead of a string?
   * Because it's theoretically possible to send headers repeated with different
   * values, though I'm not sure if this is in common use.
   */
  protected $httpQueryHeaders = array();

  /**
   * The raw, unparsed HTTP response header string.
   */
  protected $httpRawResponseHeaders;

  /**
   * An associative array of headers sent from the server as part of the server
   * response, keyed by header field name.
   *
   * Like with BetterGetterHttp::httpQueryHeaders, each value is a flat array of
   * values, since the same field with different browsers may be sent by
   * servers. Unlike with outgoing headers, this practice *is* in common use,
   * particularly when it comes to cookies.
   * 
   * Note that this is not populated immediately after a response is returned;
   * it's populated by the parsehttpResponseHeaders() method, which will be called if
   * the getResponseHeaders() or getResponseHeader() methods are called and the
   * headers haven't been parsed yet. This makes it possible to quickly get an
   * HTTP response without the overhead of parsing the headers if they're not
   * needed.
   *
   * @see http://en.wikipedia.org/wiki/HTTP_cookie#Setting_a_cookie
   */
  protected $httpResponseHeaders;

  /**
   * A string containing the HTTP response, minus headers.
   */
  protected $httpResponseBody;

  /**
   * An array of HTTP status information.
   *
   * The associative array will have the following keys:
   * - raw: The raw status header, eg "HTTP/1.1 404 Not Found"
   * - version: The HTTP version, eg "HTTP/1.1"
   * - code: The numeric status code, eg "404"
   * - status: The textual status description, eg "Not Found"
   */
  protected $httpStatus;

  /**
   * Override the setCurlOpt method to stop clever people from using it to set
   * HTTP headers
   *
   * @throws BetterGetterConfigException
   */
  public function setCurlOpt($option, $value) {
    if ($option === CURLOPT_HTTPHEADER) {
      throw new BetterGetterConfigException('Please use the setHttpHeader() or setHttpHeaders() methods to set HTTP headers.');
    }
    return parent::setCurlOpt($option, $value);
  }

  /**
   * Get an outgoing HTTP header's value.
   *
   * @param $header
   *   The outgoing header.
   * @return
   *   The values currently being stored for the header, in a one-dimensional
   *   array; or NULL if none have been defined yet.
   */
  public function getQueryHeader($header) {
    return isset($this->httpQueryHeaders[$header]) ? $this->httpQueryHeaders[$header] : NULL;
  }

  /**
   * Set an outgoing HTTP header.
   *
   * @param $header
   *   The header to set a value for.
   * @param $value
   *   The value to be set for the given header. For convenience, this may be
   *   either a single value or an array; the former for simplicity's sake, and
   *   the latter when it is occasionally necessary.
   * @return
   *   $this, for chaining purposes
   *
   * @see BetterGetterHttp::httpQueryHeaders
   */
  public function setQueryHeader($header, $value) {
    if (is_array($value)) {
      $this->httpQueryHeaders[$header] = $value;
    }
    else {
      $this->httpQueryHeaders[$header] = array($value);
    }
    return $this;
  }

  /**
   * Get all outgoing HTTP headers.
   *
   * @return
   *   An associative array keyed by HTTP headers, with values being
   *   one-dimensional arrays of values for that header.
   */
  public function getQueryHeaders() {
    return $this->httpQueryHeaders;
  }

  /**
   * Set multiple outgoing HTTP headers.
   *
   * @param $headers
   *   An associative array keyed by HTTP header, with each value being either a
   *   single value or a one-dimensional array of values for the header.
   * @return
   *   $this, for chaining.
   *
   * @see BetterGetterHttp::setHttpHeader()
   */
  public function setQueryHeaders($headers) {
    foreach ($headers as $header => $value) {
      $this->setHttpHeader($header, $value);
    }
    return $this;
  }

  /**
   * Get the respose body of a successful HTTP query.
   *
   * @return
   *   A string containing the response body of the query (or NULL if the cURL
   *   handle hasn't been executed yet).
   */
  public function getResponseBody() {
    return $this->httpResponseBody;
  }

  /**
   * Get all headers returned from a successful HTTP query.
   *
   * @return
   *   An array keyed by header, with the value being a one-dimensional array
   *   of values returned for that header.
   *
   * @see BetterGetterHttp::httpResponseHeaders
   */
  public function getResponseHeaders() {
    // If we have a response, but the response headers haven't been parsed yet…
    if ($this->rawResponse !== NULL && $this->httpResponseHeaders === NULL) {
      $this->parseResponseHeaders();
    }
    return $this->httpResponseHeaders;
  }

  /**
   * Get all values for a particular HTTP header from an HTTP query response.
   *
   * @param $header
   *   The header to return values for.
   * @return
   *   A one-dimensional array of values the server sent for that header; or
   *   NULL if none.
   *
   * @see BetterGetterHttp::httpResponseHeaders
   */
  public function getResponseHeader($header) {
    $headers = $this->getResponseHeaders();
    return isset($headers[$header]) ? $headers[$header] : NULL;
  }

  /**
   * Get all or part of the HTTP status of the query response.
   *
   * @param $part
   *   The part of the response to return, which is one of:
   *   - raw: The raw status header, eg "HTTP/1.1 404 Not Found"
   *   - version: The HTTP version, eg "HTTP/1.1"
   *   - code: The numeric status code, eg "404"
   *   - status: The textual status description, eg "Not Found"
   *   - NULL: An associative array with keys and values as above.
   * @return
   *   Either a string containing the part of the HTTP status as selected by the
   *   $part parameter, or an array containing all parts if $part is NULL.
   *
   * @see BetterGetterHttp::httpStatus
   */
  public function getResponseStatus($part = NULL) {
    return empty($part) ? $this->httpStatus : $this->httpStatus[$part];
  }

  /**
   * Format the outgoing HTTP header array into a format the cURL library is
   * expecting and assign them as a cURL option.
   *
   * This method should probably be called right before the cURL handle is
   * executed.
   *
   * @return
   *   $this, for chaining.
   */
  public function prepareHttpHeaders() {
    $headers = array();
    foreach ($this->httpQueryHeaders as $header => $values) {
      foreach ($values as $value) {
        $headers[] = $header . ': ' . $value;
      }
    }
    $this->curlOpts[CURLOPT_HTTPHEADER] = $headers;
    $this->curlOpts[CURLOPT_HEADER] = TRUE;
    // Get the outgoing query string so we can verify what our outgoing headers
    // were.

    return $this;
  }

  /**
   * Prepare and execute the cURL handle and parse the response.
   *
   * @return
   *   $this, for chaining.
   */
  public function execute() {
    return $this->prepareHttpHeaders()->prepareCurlHandle()->executeCurlHandle()->splitResponse();
  }

  /**
   * Split the HTTP response into body and header parts and handle both
   * appropriately.
   *
   * @return
   *   $this, for chaining.
   */
  public function splitResponse() {
    // cURL may return several sets of headers when doing HTTP authentication.
    // We'll use the last headers sent as the ones to parse.
    $chunks = preg_split('/\n\n|\r\n\r\n/', $this->rawResponse, 0, PREG_SPLIT_OFFSET_CAPTURE);
    // Find the last chunk that looks like a chunk of HTTP headers.
    foreach ($chunks as $chunk_key => $chunk) {
      // Look for a HTTP response's first line, which will look something like
      // HTTP/1.0 200 OK
      if (preg_match('/^(HTTP\/\d+\.\d+) (\d{3}) (.+)$/m', $chunk[0], $status_matches)) {
        // A failed preg_match() nulls out the variable passed to gather
        // matches, so keep track of our last success.
        $good_status_matches = $status_matches;
      }
      else {
        // Okay, if we didn't find it, then consider this chunk the first piece
        // of the body, and the previous chunk the headers to use.
        $this->httpRawResponseHeaders = $chunks[$chunk_key - 1][0];
        $this->httpResponseBody = substr($this->rawResponse, $chunk[1]);
        break;
      }
    }
    // Did we not find any plausible headers in the response?
    if (empty($this->httpRawResponseHeaders)) {
      throw new BetterGetterHttpException('Plausible HTTP headers not found.');
    }
    $this->httpStatus = array(
      'raw' => $good_status_matches[0],
      'version' => $good_status_matches[1],
      'code' => $good_status_matches[2],
      'status' => $good_status_matches[3],
    );
    return $this;
  }

  /**
   * Parse the headers of a HTTP response into an easily-accessible array.
   *
   * @return
   *   $this, for chaining.
   *
   * @see BetterGetterHttp::httpResponseHeaders
   */
  public function parseResponseHeaders() {
    $header_lines = preg_split('/\n|\r\n/', $this->httpRawResponseHeaders);
    $this->httpResponseHeaders = array();
    // The next() trick below is a silly trick to let us skip the first item
    // in the array, which will be the HTTP status line.
    while ($header_line = next($header_lines)) {
      $header_parts = explode(': ', $header_line, 2);
      if (isset($this->httpResponseHeaders[$header_parts[0]])) {
        $this->httpResponseHeaders[$header_parts[0]][] = $header_parts[1];
      }
      else {
        $this->httpResponseHeaders[$header_parts[0]] = array($header_parts[1]);
      }
    }
    return $this;
  }
}

class BetterGetterHttpException extends Exception {}