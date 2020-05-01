//
// Core client web interface functions for the Printer Application Framework
//
// Copyright © 2019-2020 by Michael R Sweet.
// Copyright © 2010-2019 by Apple Inc.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

//
// Include necessary headers...
//

#include "pappl-private.h"
#include <math.h>


//
// Local functions...
//

static char	*get_cookie(pappl_client_t *client, const char *name, char *buffer, size_t bufsize);


//
// 'papplClientGetForm()' - Get POST form data from the web client.
//

int					// O - Number of form variables read
papplClientGetForm(
    pappl_client_t *client,		// I - Client
    cups_option_t  **form)		// O - Form variables
{
  const char	*content_type = httpGetField(client->http, HTTP_FIELD_CONTENT_TYPE);
					// Content-Type header
  const char	*boundary;		// boundary value for multi-part
  char		body[65536],		// Message body
		*bodyptr,		// Pointer into message body
		*bodyend;		// End of message body
  size_t	body_size = 0;		// Size of message body
  ssize_t	bytes;			// Bytes read
  int		num_form = 0;		// Number of form variables
  http_state_t	initial_state;		// Initial HTTP state


  // Read up to 2MB of data from the client...
  *form         = NULL;
  initial_state = httpGetState(client->http);

  for (bodyptr = body, bodyend = body + sizeof(body); (bytes = httpRead2(client->http, bodyptr, bodyend - bodyptr)) > 0; bodyptr += bytes)
  {
    body_size += (size_t)bytes;

    if (body_size >= sizeof(body))
      break;
  }

  papplLogClient(client, PAPPL_LOGLEVEL_DEBUG, "Read %ld bytes of form data (%s).", (long)body_size, content_type);

  // Flush remaining data...
  if (httpGetState(client->http) == initial_state)
    httpFlush(client->http);

  // Parse the data in memory...
  bodyend = body + body_size;

  if (!strcmp(content_type, "application/x-www-form-urlencoded"))
  {
    // Read URL-encoded form data...
    char	name[64],		// Variable name
		*nameptr,		// Pointer into name
		value[1024],		// Variable value
		*valptr;		// Pointer into value

    for (bodyptr = body; bodyptr < bodyend;)
    {
      // Get the name...
      nameptr = name;
      while (bodyptr < bodyend && *bodyptr != '=')
      {
	int ch = *bodyptr++;		// Name character

	if (ch == '%' && isxdigit(bodyptr[0] & 255) && isxdigit(bodyptr[1] & 255))
	{
	  // Hex-encoded character
	  if (isdigit(*bodyptr))
	    ch = (*bodyptr++ - '0') << 4;
	  else
	    ch = (tolower(*bodyptr++) - 'a' + 10) << 4;

	  if (isdigit(*bodyptr))
	    ch |= *bodyptr++ - '0';
	  else
	    ch |= tolower(*bodyptr++) - 'a' + 10;
	}
	else if (ch == '+')
	  ch = ' ';

	if (nameptr < (name + sizeof(name) - 1))
	  *nameptr++ = ch;
      }
      *nameptr = '\0';

      if (bodyptr >= bodyend)
	break;

      // Get the value...
      bodyptr ++;
      valptr = value;
      while (bodyptr < bodyend && *bodyptr != '&')
      {
	int ch = *bodyptr++;			// Name character

	if (ch == '%' && isxdigit(bodyptr[0] & 255) && isxdigit(bodyptr[1] & 255))
	{
	  // Hex-encoded character
	  if (isdigit(*bodyptr))
	    ch = (*bodyptr++ - '0') << 4;
	  else
	    ch = (tolower(*bodyptr++) - 'a' + 10) << 4;

	  if (isdigit(*bodyptr))
	    ch |= *bodyptr++ - '0';
	  else
	    ch |= tolower(*bodyptr++) - 'a' + 10;
	}
	else if (ch == '+')
	  ch = ' ';

	if (valptr < (value + sizeof(value) - 1))
	  *valptr++ = ch;
      }
      *valptr = '\0';

      if (bodyptr < bodyend)
	bodyptr ++;

      // Add the name + value to the option array...
      num_form = cupsAddOption(name, value, num_form, form);
    }
  }
  else if (!strncmp(content_type, "multipart/form-data; ", 21) && (boundary = strstr(content_type, "boundary=")) != NULL)
  {
    // Read multi-part form data...
    char	name[1024],		// Form variable name
		filename[1024],		// Form filename
		bstring[256],		// Boundary string to look for
		*bend,			// End of value (boundary)
		*line,			// Start of line
		*ptr;			// Pointer into name/filename
    size_t	blen;			// Length of boundary string

    // Format the boundary string we are looking for...
    snprintf(bstring, sizeof(bstring), "\r\n--%s", boundary + 9);
    blen = strlen(bstring);

    // Parse lines in the message body...
    name[0] = '\0';
    filename[0] = '\0';

    for (bodyptr = body; bodyptr < bodyend;)
    {
      // Split out a line...
      for (line = bodyptr; bodyptr < bodyend; bodyptr ++)
      {
        if (!memcmp(bodyptr, "\r\n", 2))
        {
          *bodyptr = '\0';
          bodyptr += 2;
          break;
        }
      }

      if (bodyptr >= bodyend)
        break;

      papplLogClient(client, PAPPL_LOGLEVEL_DEBUG, "Line '%s'.", line);

      if (!*line)
      {
        // End of headers, grab value...
        if (!name[0])
        {
          // No name value...
	  papplLogClient(client, PAPPL_LOGLEVEL_ERROR, "Invalid multipart form data.");
	  break;
	}

	for (bend = bodyend - blen, ptr = memchr(bodyptr, '\r', bend - bodyptr); ptr; ptr = memchr(ptr + 1, '\r', bend - ptr - 1))
	{
	  // Check for boundary string...
	  if (!memcmp(ptr, bstring, blen))
	    break;
	}

	if (!ptr)
	{
	  // No boundary string, invalid data...
	  papplLogClient(client, PAPPL_LOGLEVEL_ERROR, "Invalid multipart form data.");
	  break;
	}

        // Point to the start of the boundary string...
	bend    = ptr;
        ptr     = bodyptr;
        bodyptr = bend + blen;

	if (filename[0])
	{
	  // Save an embedded file...
          const char *tempfile;		// Temporary file

	  if ((tempfile = _papplClientCreateTempFile(client, ptr, (size_t)(bend - ptr))) == NULL)
	    break;

          num_form = cupsAddOption(name, tempfile, num_form, form);
	}
	else
	{
	  // Save the form variable...
	  *bend = '\0';

          num_form = cupsAddOption(name, ptr, num_form, form);
	}

	name[0]     = '\0';
	filename[0] = '\0';

        if (bodyptr < (bodyend - 1) && bodyptr[0] == '\r' && bodyptr[1] == '\n')
          bodyptr += 2;
      }
      else if (!strncasecmp(line, "Content-Disposition:", 20))
      {
	if ((ptr = strstr(line + 20, " name=\"")) != NULL)
	{
	  strlcpy(name, ptr + 7, sizeof(name));

	  if ((ptr = strchr(name, '\"')) != NULL)
	    *ptr = '\0';
	}

	if ((ptr = strstr(line + 20, " filename=\"")) != NULL)
	{
	  strlcpy(filename, ptr + 11, sizeof(filename));

	  if ((ptr = strchr(filename, '\"')) != NULL)
	    *ptr = '\0';
	}
      }
    }
  }

  // Return whatever we got...
  return (num_form);
}


//
// 'papplClientHTMLAuthorize()' - Handle authorization for the web interface.
//
// IPP operation callbacks needing to perform authorization should use the
// @link papplClientIsAuthorized@ function instead.
//

bool					// O - `true` if authorized, `false` otherwise
papplClientHTMLAuthorize(
    pappl_client_t *client)		// I - Client
{
  char		auth_cookie[65],	// Authorization cookie
		session_key[65],	// Current session key
		password_hash[100],	// Password hash
		auth_text[256];		// Authorization string
  unsigned char	auth_hash[32];		// Authorization hash
  const char	*status = NULL;		// Status message, if any


  // Don't authorize if we have no auth service or we don't have a password set.
  if (!client || (!client->system->auth_service && !client->system->password_hash[0]))
    return (true);

  // When using an auth service, use HTTP Basic authentication...
  if (client->system->auth_service)
  {
    http_status_t code = papplClientIsAuthorized(client);

    if (code != HTTP_STATUS_CONTINUE)
    {
      papplClientRespondHTTP(client, code, NULL, NULL, 0, 0);
      return (false);
    }
    else
      return (true);
  }

  // Otherwise look for the authorization cookie...
  if (get_cookie(client, "auth", auth_cookie, sizeof(auth_cookie)))
  {
    snprintf(auth_text, sizeof(auth_text), "%s:%s", papplSystemGetSessionKey(client->system, session_key, sizeof(session_key)), papplSystemGetPassword(client->system, password_hash, sizeof(password_hash)));
    cupsHashData("sha2-256", (unsigned char *)auth_text, strlen(auth_text), auth_hash, sizeof(auth_hash));
    cupsHashString(auth_hash, sizeof(auth_hash), auth_text, sizeof(auth_text));

    if (!strcmp(auth_cookie, auth_text))
    {
      // Hashes match so we are authorized.  Use "web-admin" as the username.
      strlcpy(client->username, "web-admin", sizeof(client->username));

      return (true);
    }
  }

  // No cookie, so see if this is a form submission...
  if (client->operation == HTTP_STATE_POST)
  {
    // Yes, grab the login information and try to authorize...
    int			num_form = 0;	// Number of form variable
    cups_option_t	*form = NULL;	// Form variables
    const char		*password;	// Password from user

    if ((num_form = papplClientGetForm(client, &form)) == 0)
    {
      status = "Invalid form data.";
    }
    else if (!papplClientValidateForm(client, num_form, form))
    {
      status = "Invalid form submission.";
    }
    else if ((password = cupsGetOption("password", num_form, form)) == NULL)
    {
      status = "Login password required.";
    }
    else
    {
      // Hash the user-supplied password with the salt from the stored password
      papplSystemGetPassword(client->system, password_hash, sizeof(password_hash));
      papplSystemHashPassword(client->system, password_hash, password, auth_text, sizeof(auth_text));

      if (!strncmp(password_hash, auth_text, strlen(password_hash)))
      {
        // Password hashes match, generate the cookie from the session key and
        // password hash...
        char	cookie[256],		// New authorization cookie
		expires[64];		// Expiration date/time

	snprintf(auth_text, sizeof(auth_text), "%s:%s", papplSystemGetSessionKey(client->system, session_key, sizeof(session_key)), password_hash);
	cupsHashData("sha2-256", (unsigned char *)auth_text, strlen(auth_text), auth_hash, sizeof(auth_hash));
	cupsHashString(auth_hash, sizeof(auth_hash), auth_text, sizeof(auth_text));

	snprintf(cookie, sizeof(cookie), "auth=%s; path=/; expires=%s; httponly; secure;", auth_text, httpGetDateString2(time(NULL) + 3600, expires, sizeof(expires)));
        httpSetCookie(client->http, cookie);
      }
      else
      {
        status = "Password incorrect.";
      }
    }

    cupsFreeOptions(num_form, form);

    // Make the caller think this is a GET request...
    client->operation = HTTP_STATE_GET;

    if (!status)
    {
      // Hashes match so we are authorized.  Use "web-admin" as the username.
      strlcpy(client->username, "web-admin", sizeof(client->username));

      return (true);
    }
  }

  // If we get this far, show the standard login form...
  papplClientRespondHTTP(client, HTTP_STATUS_OK, NULL, "text/html", 0, 0);
  papplClientHTMLHeader(client, "Login", 0);
  papplClientHTMLPuts(client,
                      "    <div class=\"content\">\n"
                      "      <div class=\"row\">\n"
		      "        <div class=\"col-12\">\n"
		      "          <h1 class=\"title\">Login</h1>\n");

  if (status)
    papplClientHTMLPrintf(client, "          <div class=\"banner\">%s</div>\n", status);

  papplClientHTMLStartForm(client, client->uri, false);
  papplClientHTMLPuts(client,
                      "          <p><label>Password: <input type=\"password\" name=\"password\"></label> <input type=\"submit\" value=\"Login\"></p>\n"
                      "          </form>\n"
                      "        </div>\n"
                      "      </div>\n");
  papplClientHTMLFooter(client);

  return (false);
}


//
// 'papplClientHTMLEscape()' - Write a HTML-safe string.
//

void
papplClientHTMLEscape(
    pappl_client_t *client,		// I - Client
    const char     *s,			// I - String to write
    size_t         slen)		// I - Number of characters to write (`0` for nul-terminated)
{
  const char	*start,			// Start of segment
		*end;			// End of string


  start = s;
  end   = s + (slen > 0 ? slen : strlen(s));

  while (*s && s < end)
  {
    if (*s == '&' || *s == '<' || *s == '\"')
    {
      if (s > start)
        httpWrite2(client->http, start, (size_t)(s - start));

      if (*s == '&')
        httpWrite2(client->http, "&amp;", 5);
      else if (*s == '<')
        httpWrite2(client->http, "&lt;", 4);
      else
        httpWrite2(client->http, "&quot;", 6);

      start = s + 1;
    }

    s ++;
  }

  if (s > start)
    httpWrite2(client->http, start, (size_t)(s - start));
}


//
// 'papplClientHTMLFooter()' - Show the web interface footer.
//
// This function also writes the trailing 0-length chunk.
//

void
papplClientHTMLFooter(
    pappl_client_t *client)		// I - Client
{
  const char *footer = papplSystemGetFooterHTML(papplClientGetSystem(client));
					// Footer HTML

  if (footer)
  {
    papplClientHTMLPuts(client,
                        "    <div class=\"footer\">\n"
                        "      <div class=\"row\">\n"
                        "        <div class=\"col-12\">");
    papplClientHTMLPuts(client, footer);
    papplClientHTMLPuts(client,
                        "</div>\n"
                        "      </div>\n"
                        "    </div>\n");
  }

  papplClientHTMLPuts(client,
		      "  </body>\n"
		      "</html>\n");
  httpWrite2(client->http, "", 0);
}


//
// 'papplClientHTMLHeader()' - Show the web interface header and title.
//

void
papplClientHTMLHeader(
    pappl_client_t *client,		// I - Client
    const char     *title,		// I - Title
    int            refresh)		// I - Refresh timer, if any
{
  pappl_system_t	*system = client->system;
					// System
  pappl_printer_t	*printer;	// Printer
  _pappl_resource_t	*r;		// Current resource
  const char		*name;		// Name for title/header


  if ((system->options & PAPPL_SOPTIONS_MULTI_QUEUE) || (printer = (pappl_printer_t *)cupsArrayFirst(system->printers)) == NULL)
    name = system->name;
  else
    name = printer->name;

  papplClientHTMLPrintf(client,
			"<!DOCTYPE html>\n"
			"<html>\n"
			"  <head>\n"
			"    <title>%s%s%s</title>\n"
			"    <link rel=\"shortcut icon\" href=\"/favicon.png\" type=\"image/png\">\n"
			"    <link rel=\"stylesheet\" href=\"/style.css\">\n"
			"    <meta http-equiv=\"X-UA-Compatible\" content=\"IE=9\">\n", title ? title : "", title ? " - " : "", name);
  if (refresh > 0)
    papplClientHTMLPrintf(client, "<meta http-equiv=\"refresh\" content=\"%d\">\n", refresh);
  papplClientHTMLPrintf(client,
			"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
			"  </head>\n"
			"  <body>\n"
			"    <div class=\"header\">\n"
			"      <div class=\"row\">\n"
			"        <div class=\"col-12 nav\">\n"
			"          <a class=\"btn\" href=\"/\"><img src=\"/navicon.png\"> %s</a>\n", name);

  pthread_rwlock_rdlock(&system->rwlock);

  for (r = (_pappl_resource_t *)cupsArrayFirst(system->resources); r; r = (_pappl_resource_t *)cupsArrayNext(system->resources))
  {
    if (r->label)
    {
      if (strcmp(client->uri, r->path))
      {
        if (r->secure)
          papplClientHTMLPrintf(client, "          <a class=\"btn\" href=\"https://%s:%d%s\">%s</a>\n", client->host_field, client->host_port, r->path, r->label);
	else
          papplClientHTMLPrintf(client, "          <a class=\"btn\" href=\"%s\">%s</a>\n", r->path, r->label);
      }
      else
        papplClientHTMLPrintf(client, "          <span class=\"active\">%s</span>\n", r->label);
    }
  }

  pthread_rwlock_unlock(&system->rwlock);

  papplClientHTMLPuts(client,
		      "        </div>\n"
		      "      </div>\n"
		      "    </div>\n");
}


//
// '_papplCLientHTMLInfo()' - Show system/printer information.
//

void
_papplClientHTMLInfo(
    pappl_client_t  *client,		// I - Client
    bool            is_form,		// I - `true` to show a form, `false` otherwise
    const char      *dns_sd_name,	// I - DNS-SD name, if any
    const char      *location,		// I - Location, if any
    const char      *geo_location,	// I - Geo-location, if any
    const char      *organization,	// I - Organization, if any
    const char      *org_unit,		// I - Organizational unit, if any
    pappl_contact_t *contact)		// I - Contact, if any
{
  double	lat = 0.0, lon = 0.0;	// Latitude and longitude in degrees


  if (geo_location)
    sscanf(geo_location, "geo:%lf,%lf", &lat, &lon);

  if (is_form)
    papplClientHTMLStartForm(client, client->uri, false);

  // DNS-SD name...
  papplClientHTMLPuts(client,
		      "          <table class=\"form\">\n"
		      "            <tbody>\n"
		      "              <tr><th>Name:</th><td>");
  if (is_form)
    papplClientHTMLPrintf(client, "<input type=\"text\" name=\"dns_sd_name\" value=\"%s\" placeholder=\"DNS-SD Service Name\">", dns_sd_name ? dns_sd_name : "");
  else
    papplClientHTMLEscape(client, dns_sd_name ? dns_sd_name : "Not set", 0);

  // Location and geo-location...
  papplClientHTMLPuts(client,
                      "</td></tr>\n"
		      "              <tr><th>Location:</th><td>");
  if (is_form)
  {
    papplClientHTMLPrintf(client,
                          "<input type=\"text\" name=\"location\" value=\"%s\" placeholder=\"Human-Readable Location\"><br>\n"
                          "<input type=\"number\" name=\"geo_location_lat\" min=\"-90\" max=\"90\" step=\"0.0001\" value=\"%.4f\" onChange=\"updateMap();\">&nbsp;&deg;&nbsp;latitude x <input type=\"number\" name=\"geo_location_lon\" min=\"-180\" max=\"180\" step=\"0.0001\" value=\"%.4f\" onChange=\"updateMap();\">&nbsp;&deg;&nbsp;longitude <button id=\"geo_location_lookup\" onClick=\"event.preventDefault(); navigator.geolocation.getCurrentPosition(setGeoLocation);\">Use My Position</button>", location ? location : "", lat, lon);
  }
  else
  {
    papplClientHTMLPrintf(client, "%s", location ? location : "Not set");
    if (geo_location)
      papplClientHTMLPrintf(client, "<br>\n%g&deg;&nbsp;%c&nbsp;latitude x %g&deg;&nbsp;%c&nbsp;longitude", fabs(lat), lat < 0.0 ? 'S' : 'N', fabs(lon), lon < 0.0 ? 'W' : 'E');
  }

  // Show an embedded map of the location...
  if (geo_location || is_form)
    papplClientHTMLPrintf(client,
			  "<br>\n"
			  "<iframe id=\"map\" frameborder=\"0\" scrolling=\"no\" marginheight=\"0\" marginwidth=\"0\" src=\"https://www.openstreetmap.org/export/embed.html?bbox=%g,%g,%g,%g&amp;layer=mapnik&amp;marker=%g,%g\"></iframe>", lon - 0.00025, lat - 0.00025, lon + 0.00025, lat + 0.00025, lat, lon);

  // Organization
  papplClientHTMLPuts(client,
                      "</td></tr>\n"
		      "              <tr><th>Organization:</th><td>");

  if (is_form)
    papplClientHTMLPrintf(client,
                          "<input type=\"text\" name=\"organization\" placeholder=\"Organization Name\" value=\"%s\"><br>\n"
                          "<input type=\"text\" name=\"organizational_unit\" placeholder=\"Organizational Unit\" value=\"%s\">", organization ? organization : "", org_unit ? org_unit : "");
  else
    papplClientHTMLPrintf(client, "%s%s%s", organization ? organization : "Unknown", org_unit ? ", " : "", org_unit ? org_unit : "");

  // Contact
  papplClientHTMLPuts(client,
                      "</td></tr>\n"
                      "              <tr><th>Contact:</th><td>");

  if (is_form)
  {
    papplClientHTMLPrintf(client,
                          "<input type=\"text\" name=\"contact_name\" placeholder=\"Name\" value=\"%s\"><br>\n"
                          "<input type=\"email\" name=\"contact_email\" placeholder=\"name@domain\" value=\"%s\"><br>\n"
                          "<input type=\"tel\" name=\"contact_telephone\" placeholder=\"867-5309\" value=\"%s\"></td></tr>\n"
		      "              <tr><th></th><td><input type=\"submit\" value=\"Save Changes\">", contact->name, contact->email, contact->telephone);
  }
  else if (contact->email[0])
  {
    papplClientHTMLPrintf(client, "<a href=\"mailto:%s\">%s</a>", contact->email, contact->name[0] ? contact->name : contact->email);

    if (contact->telephone[0])
      papplClientHTMLPrintf(client, "<br><a href=\"tel:%s\">%s</a>", contact->telephone, contact->telephone);
  }
  else if (contact->name[0])
  {
    papplClientHTMLEscape(client, contact->name, 0);

    if (contact->telephone[0])
      papplClientHTMLPrintf(client, "<br><a href=\"tel:%s\">%s</a>", contact->telephone, contact->telephone);
  }
  else if (contact->telephone[0])
  {
    papplClientHTMLPrintf(client, "<a href=\"tel:%s\">%s</a>", contact->telephone, contact->telephone);
  }
  else
    papplClientHTMLPuts(client, "Not set");

  papplClientHTMLPuts(client,
		      "</td></tr>\n"
		      "            </tbody>\n"
		      "          </table>\n");

  if (is_form)
  {
    // The following Javascript updates the map and lat/lon fields.
    //
    // Note: I should probably use the Openstreetmap Javascript API so that
    // the marker position gets updated.  Right now I'm setting the marker
    // value in the URL but the OSM simple embedding URL doesn't update the
    // marker position after the page is loaded...
    papplClientHTMLPuts(client,
                        "          </form>\n"
                        "          <script>\n"
                        "function updateMap() {\n"
                        "  let map = document.getElementById('map');\n"
                        "  let lat = parseFloat(document.forms['form']['geo_location_lat'].value);\n"
                        "  let lon = parseFloat(document.forms['form']['geo_location_lon'].value);\n"
                        "  let bboxl = (lon - 0.00025).toFixed(4);\n"
                        "  let bboxb = (lat - 0.00025).toFixed(4);\n"
                        "  let bboxr = (lon + 0.00025).toFixed(4);\n"
                        "  let bboxt = (lat + 0.00025).toFixed(4);\n"
                        "  map.src = 'https://www.openstreetmap.org/export/embed.html?bbox=' + bboxl + ',' + bboxb + ',' + bboxr + ',' + bboxt + '&amp;layer=mapnik&amp;marker=' + lat + ',' + lon;\n"
                        "}\n"
                        "function setGeoLocation(p) {\n"
                        "  let lat = p.coords.latitude.toFixed(4);\n"
                        "  let lon = p.coords.longitude.toFixed(4);\n"
                        "  document.forms['form']['geo_location_lat'].value = lat;\n"
                        "  document.forms['form']['geo_location_lon'].value = lon;\n"
                        "  updateMap();\n"
                        "}\n"
                        "</script>\n");
  }
}


//
// 'papplClientHTMLPrintf()' - Send formatted text to the client, quoting as needed.
//

void
papplClientHTMLPrintf(
    pappl_client_t *client,		// I - Client
    const char     *format,		// I - Printf-style format string
    ...)				// I - Additional arguments as needed
{
  va_list	ap;			// Pointer to arguments
  const char	*start;			// Start of string
  char		size,			// Size character (h, l, L)
		type;			// Format type character
  int		width,			// Width of field
		prec;			// Number of characters of precision
  char		tformat[100],		// Temporary format string for sprintf()
		*tptr,			// Pointer into temporary format
		temp[1024];		// Buffer for formatted numbers
  char		*s;			// Pointer to string


  // Loop through the format string, formatting as needed...
  va_start(ap, format);
  start = format;

  while (*format)
  {
    if (*format == '%')
    {
      if (format > start)
        httpWrite2(client->http, start, (size_t)(format - start));

      tptr    = tformat;
      *tptr++ = *format++;

      if (*format == '%')
      {
        httpWrite2(client->http, "%", 1);
        format ++;
	start = format;
	continue;
      }
      else if (strchr(" -+#\'", *format))
        *tptr++ = *format++;

      if (*format == '*')
      {
        // Get width from argument...
	format ++;
	width = va_arg(ap, int);

	snprintf(tptr, sizeof(tformat) - (size_t)(tptr - tformat), "%d", width);
	tptr += strlen(tptr);
      }
      else
      {
	width = 0;

	while (isdigit(*format & 255))
	{
	  if (tptr < (tformat + sizeof(tformat) - 1))
	    *tptr++ = *format;

	  width = width * 10 + *format++ - '0';
	}
      }

      if (*format == '.')
      {
	if (tptr < (tformat + sizeof(tformat) - 1))
	  *tptr++ = *format;

        format ++;

        if (*format == '*')
	{
          // Get precision from argument...
	  format ++;
	  prec = va_arg(ap, int);

	  snprintf(tptr, sizeof(tformat) - (size_t)(tptr - tformat), "%d", prec);
	  tptr += strlen(tptr);
	}
	else
	{
	  prec = 0;

	  while (isdigit(*format & 255))
	  {
	    if (tptr < (tformat + sizeof(tformat) - 1))
	      *tptr++ = *format;

	    prec = prec * 10 + *format++ - '0';
	  }
	}
      }

      if (*format == 'l' && format[1] == 'l')
      {
        size = 'L';

	if (tptr < (tformat + sizeof(tformat) - 2))
	{
	  *tptr++ = 'l';
	  *tptr++ = 'l';
	}

	format += 2;
      }
      else if (*format == 'h' || *format == 'l' || *format == 'L')
      {
	if (tptr < (tformat + sizeof(tformat) - 1))
	  *tptr++ = *format;

        size = *format++;
      }
      else
        size = 0;

      if (!*format)
      {
        start = format;
        break;
      }

      if (tptr < (tformat + sizeof(tformat) - 1))
        *tptr++ = *format;

      type  = *format++;
      *tptr = '\0';
      start = format;

      switch (type)
      {
	case 'E' : // Floating point formats
	case 'G' :
	case 'e' :
	case 'f' :
	case 'g' :
	    if ((size_t)(width + 2) > sizeof(temp))
	      break;

	    sprintf(temp, tformat, va_arg(ap, double));

            httpWrite2(client->http, temp, strlen(temp));
	    break;

        case 'B' : // Integer formats
	case 'X' :
	case 'b' :
        case 'd' :
	case 'i' :
	case 'o' :
	case 'u' :
	case 'x' :
	    if ((size_t)(width + 2) > sizeof(temp))
	      break;

#  ifdef HAVE_LONG_LONG
            if (size == 'L')
	      sprintf(temp, tformat, va_arg(ap, long long));
	    else
#  endif // HAVE_LONG_LONG
            if (size == 'l')
	      sprintf(temp, tformat, va_arg(ap, long));
	    else
	      sprintf(temp, tformat, va_arg(ap, int));

            httpWrite2(client->http, temp, strlen(temp));
	    break;

	case 'p' : // Pointer value
	    if ((size_t)(width + 2) > sizeof(temp))
	      break;

	    sprintf(temp, tformat, va_arg(ap, void *));

            httpWrite2(client->http, temp, strlen(temp));
	    break;

        case 'c' : // Character or character array
            if (width <= 1)
            {
              temp[0] = (char)va_arg(ap, int);
              temp[1] = '\0';
              papplClientHTMLEscape(client, temp, 1);
            }
            else
              papplClientHTMLEscape(client, va_arg(ap, char *), (size_t)width);
	    break;

	case 's' : // String
	    if ((s = va_arg(ap, char *)) == NULL)
	      s = "(null)";

            papplClientHTMLEscape(client, s, strlen(s));
	    break;
      }
    }
    else
      format ++;
  }

  if (format > start)
    httpWrite2(client->http, start, (size_t)(format - start));

  va_end(ap);
}


//
// 'papplClientHTMLPuts()' - Write a HTML string.
//

void
papplClientHTMLPuts(
    pappl_client_t *client,		// I - Client
    const char     *s)			// I - String
{
  if (client && s && *s)
    httpWrite2(client->http, s, strlen(s));
}


//
// 'papplClientHTMLStartForm()' - Start a HTML form.
//
// This function starts a HTML form with the specified "action" path and
// includes the CSRF token as a hidden variable.
//

void
papplClientHTMLStartForm(
    pappl_client_t *client,		// I - Client
    const char     *action,		// I - Form action URL
    bool           multipart)		// I - `true` if the form allows file uploads, `false` otherwise
{
  char	token[256];			// CSRF token


  if (multipart)
  {
    // When allowing file attachments, the maximum size is 1MB...
    papplClientHTMLPrintf(client,
			  "          <form action=\"%s\" id=\"form\" method=\"POST\" enctype=\"multipart/form-data\">\n"
			  "          <input type=\"hidden\" name=\"session\" value=\"%s\">\n"
			  "          <input type=\"hidden\" name=\"MAX_FILE_SIZE\" value=\"1048576\">\n", action, papplClientGetCSRFToken(client, token, sizeof(token)));
  }
  else
  {
    papplClientHTMLPrintf(client,
			  "          <form action=\"%s\" id=\"form\" method=\"POST\">\n"
			  "          <input type=\"hidden\" name=\"session\" value=\"%s\">\n", action, papplClientGetCSRFToken(client, token, sizeof(token)));
  }
}


//
// 'papplClientValidateForm()' - Validate HTML form variables.
//
// This function validates the contents of a POST form using the CSRF token
// included as a hidden variable.
//
// Note: Callers are expected to validate all other form variables.
//

bool					// O - `true` if the CSRF token is valid, `false` otherwise
papplClientValidateForm(
    pappl_client_t *client,		// I - Client
    int            num_form,		// I - Number of form variables
    cups_option_t  *form)		// I - Form variables
{
  char		token[256];		// Expected CSRF token
  const char	*session;		// Form variable


  if ((session = cupsGetOption("session", num_form, form)) == NULL)
    return (false);

  return (!strcmp(session, papplClientGetCSRFToken(client, token, sizeof(token))));
}


//
// 'get_cookie()' - Get a cookie from the client.
//

static char *				// O - Cookie value or `NULL` if not set
get_cookie(pappl_client_t *client,	// I - Client
	   const char     *name,	// I - Name of cookie
	   char           *buffer,	// I - Value buffer
	   size_t         bufsize)	// I - Size of value buffer
{
  const char	*cookie = httpGetCookie(client->http);
					// Cookies from client
  char		temp[256],		// Temporary string
		*ptr,			// Pointer into temporary string
	        *end;			// End of temporary string
  bool		found;			// Did we find it?


  // Make sure the buffer is initialize, and return if we don't have any
  // cookies...
  *buffer = '\0';

  if (!cookie)
    return (NULL);

  // Scan the cookie string for 'name=value' or 'name="value"', separated by
  // semicolons...
  while (*cookie)
  {
    while (*cookie && isspace(*cookie & 255))
      cookie ++;

    if (!*cookie)
      break;

    for (ptr = temp, end = temp + sizeof(temp) - 1; *cookie && *cookie != '='; cookie ++)
    {
      if (ptr < end)
        *ptr++ = *cookie;
    }

    if (*cookie == '=')
    {
      cookie ++;
      *ptr = '\0';
      found = !strcmp(temp, name);

      if (found)
      {
        ptr = buffer;
        end = buffer + bufsize - 1;
      }
      else
      {
        ptr = temp;
        end = temp + sizeof(temp) - 1;
      }

      if (*cookie == '\"')
      {
        for (cookie ++; *cookie && *cookie != '\"'; cookie ++)
        {
          if (ptr < end)
            *ptr++ = *cookie;
	}

	if (*cookie == '\"')
	  cookie ++;
      }
      else
      {
        for (; *cookie && *cookie != ';'; cookie ++)
        {
          if (ptr < end)
            *ptr++ = *cookie;
        }
      }

      *ptr = '\0';

      if (found)
        return (buffer);
      else if (*cookie == ';')
        cookie ++;
    }
  }

  return (NULL);
}
