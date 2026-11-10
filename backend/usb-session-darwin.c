/* SPDX-License-Identifier: Apache-2.0 */

/* Included by CUPS's Darwin USB backend to reuse its device lookup and
 * Apple USB class driver. USB operations stay in the session service. */
static char session_uri[2048];
static char session_device_id[2048];
static UInt8 session_in_pipe, session_out_pipe;
static UInt16 session_in_packet;

static void session_usb_close(void)
{
	registry_close();
	if (g.printer_obj)
		IOObjectRelease(g.printer_obj);
	if (g.make)
		CFRelease(g.make);
	if (g.model)
		CFRelease(g.model);
	if (g.serial)
		CFRelease(g.serial);
	g.printer_obj = IO_OBJECT_NULL;
	g.make = g.model = g.serial = NULL;
	session_uri[0] = '\0';
}

static ssize_t session_usb_read(void *buffer, size_t size)
{
	printer_interface_t interface = (*g.classdriver)->interface;
	/* One USB packet at a time so a timeout never loses part of a reply. */
	UInt32 count = session_in_packet;
	if (count > size)
		return -1;
	IOReturn status = (*interface)->ReadPipeTO(interface, session_in_pipe, buffer, &count, 50, 100);
	if (status == kIOUSBTransactionTimeout || status == kIOReturnTimeout)
		return 0;
	if (status == kIOReturnSuccess)
		return count;
	fprintf(stderr, "DEBUG: CAPT: USB read failed (0x%08x)\n", (unsigned)status);
	session_usb_close();
	return -1;
}

static int session_usb_write(const void *buffer, size_t size)
{
	printer_interface_t interface = (*g.classdriver)->interface;
	IOReturn status = (*interface)->WritePipeTO(interface, session_out_pipe, (void *)buffer,
		(UInt32)size, 5000, 5000);
	if (status == kIOReturnSuccess)
		return 0;
	/* A timed-out write can have partially reached the printer. Never replay it. */
	fprintf(stderr, "ERROR: CAPT: USB write failed (0x%08x)\n", (unsigned)status);
	session_usb_close();
	return -1;
}

static Boolean session_find_device(io_service_t obj, printer_interface_t interface, void *userdata)
{
	if (!obj)
		return false;
	UInt16 vendor = 0, product = 0;
	(*interface)->GetDeviceVendor(interface, &vendor);
	(*interface)->GetDeviceProduct(interface, &product);
	if (vendor != 0x04a9 || product != 0x2676)
		return true;
	UInt32 location;
	if (g.location && (!g.serial || !CFStringGetLength(g.serial)) &&
	    ((*interface)->GetLocationID(interface, &location) != kIOReturnSuccess || location != g.location))
		return true;
	return find_device_cb(obj, interface, userdata);
}

static int session_usb_open(const char *uri)
{
	char scheme[32], user[256], host[256], resource[2048];
	int port;
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), user, sizeof(user),
	                    host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_OK ||
	    strcmp(scheme, "canon2900usb") || strcasecmp(host, "Canon") || user[0] || port)
		return CUPS_BACKEND_STOP;
	char *options = strchr(resource, '?');
	if (options)
		*options++ = '\0';
	if (strcmp(resource, "/LBP2900"))
		return CUPS_BACKEND_STOP;

	char serial[1024];
	Boolean wait_eof;
	parse_options(options, serial, sizeof(serial), &g.location, &wait_eof);
	g.make = CFSTR("Canon");
	g.model = CFSTR("LBP2900");
	g.serial = cfstr_create_trim(serial);
	iterate_printers(session_find_device, NULL);
	CFStringRef driver_path = NULL;
	if (!g.printer_obj || registry_open(&driver_path) != kIOReturnSuccess || !g.bidi_flag) {
		session_usb_close();
		return CUPS_BACKEND_RETRY;
	}

	printer_interface_t interface = (*g.classdriver)->interface;
	UInt8 endpoints = 0;
	(*interface)->GetNumEndpoints(interface, &endpoints);
	session_in_pipe = session_out_pipe = 0;
	for (unsigned pipe = 1; pipe <= endpoints; ++pipe) {
		UInt8 direction, number, type, interval;
		UInt16 packet_size;
		if ((*interface)->GetPipeProperties(interface, pipe, &direction, &number, &type, &packet_size,
		                                    &interval) != kIOReturnSuccess || type != kUSBBulk)
			continue;
		if (direction == kUSBIn && packet_size && packet_size <= 512) {
			session_in_pipe = pipe;
			session_in_packet = packet_size;
		} else if (direction == kUSBOut) {
			session_out_pipe = pipe;
		}
	}
	cups_sc_status_t status;
	int size = sizeof(session_device_id);
	get_device_id(&status, session_device_id, &size);
	if (!session_in_pipe || !session_out_pipe || !size) {
		session_usb_close();
		return CUPS_BACKEND_RETRY;
	}
	strlcpy(session_uri, uri, sizeof(session_uri));
	fputs("DEBUG: CAPT: opened persistent USB session\n", stderr);
	return CUPS_BACKEND_OK;
}

static int session_usb_prepare(const char *uri, char *device_id, int size)
{
	if (g.classdriver && strcmp(session_uri, uri))
		session_usb_close();
	if (g.classdriver) {
		/* Drop unread USB data from the preceding job before connecting its
		 * replacement. A dead interface is closed and rediscovered below. */
		char stale[512];
		for (unsigned attempt = 0; attempt < 16; ++attempt) {
			ssize_t bytes = session_usb_read(stale, sizeof(stale));
			if (bytes == 0) {
				strlcpy(device_id, session_device_id, size);
				fputs("DEBUG: CAPT: reused persistent USB session\n", stderr);
				return CUPS_BACKEND_OK;
			}
			if (bytes < 0)
				break;
		}
		if (g.classdriver) {
			fputs("ERROR: CAPT: USB reply stream did not become idle between jobs\n", stderr);
			session_usb_close();
		}
	}
	int status = session_usb_open(uri);
	if (status == CUPS_BACKEND_OK)
		strlcpy(device_id, session_device_id, size);
	return status;
}
