#include <uwsgi.h>

#include <libxslt/xsltutils.h>
#include <libxslt/transform.h>

/*

	XSLT request plugin

	it takes an XML file as input (taken from DOCUMENT_ROOT + PATH_INFO by default)
	it search for an XSLT stylesheet (by default in DOCUMENT_ROOT + PATH_INFO - ext + .xsl|.xslt)
	it applies params (by default taken from the QUERY_STRING)

	XSLT routing instruction

	xslt:doc=<path1>,stylesheet=<path2>,params=<params>

*/

struct uwsgi_xslt_config {
	struct uwsgi_string_list *docroot;
	struct uwsgi_string_list *ext;
	struct uwsgi_string_list *var;
	struct uwsgi_string_list *stylesheet;
	char *content_type;
	uint16_t content_type_len;
} uxslt;

struct uwsgi_option uwsgi_xslt_options[] = {
	{"xslt-docroot", required_argument, 0, "add a document_root for xslt processing", uwsgi_opt_add_string_list, &uxslt.docroot, 0},
	{"xslt-ext", required_argument, 0, "search for xslt stylesheets with the specified extension", uwsgi_opt_add_string_list, &uxslt.ext, 0},
	{"xslt-var", required_argument, 0, "get the xslt stylesheet path from the specified request var", uwsgi_opt_add_string_list, &uxslt.var, 0},
	{"xslt-stylesheet", required_argument, 0, "if no xslt stylesheet file can be found, use the specified one", uwsgi_opt_add_string_list, &uxslt.stylesheet, 0},
	{"xslt-content-type", required_argument, 0, "set the content-type for the xslt rsult (default: text/html)", uwsgi_opt_set_str, &uxslt.content_type, 0},
	{NULL, 0, 0, NULL, NULL, NULL, 0},
};

static char *uwsgi_xslt_apply(char *xmlfile, char *xsltfile, char **params, int *rlen) {

	// we reset them every time to avoid collision with other xml engines
	xmlSubstituteEntitiesDefault(1);
	xmlLoadExtDtdDefaultValue = 1;

        xmlDocPtr doc = xmlParseFile(xmlfile);
        if (!doc) {
		return NULL;
	}

        xsltStylesheetPtr ss = xsltParseStylesheetFile((const xmlChar *) xsltfile);
        if (!ss) {
		xmlFreeDoc(doc);
                return NULL;
        }

        xmlDocPtr res = xsltApplyStylesheet(ss, doc, (const char **) params);
	if (!res) {
		xsltFreeStylesheet(ss);
		xmlFreeDoc(doc);
		return NULL;
	}

        xmlChar *output;
        int ret = xsltSaveResultToString(&output, rlen, res, ss);
	xsltFreeStylesheet(ss);
	xmlFreeDoc(res);
	xmlFreeDoc(doc);
	if (ret < 0) return NULL;
	return (char *) output;
}

static int uwsgi_request_xslt(struct wsgi_request *wsgi_req) {

	char *xmlfile = NULL;
	char *output = NULL;
        int output_rlen = 0;

	char filename[PATH_MAX+1];
	size_t filename_len = 0;
	char stylesheet[PATH_MAX+1];
	size_t stylesheet_len = 0;

	if (uwsgi_parse_vars(wsgi_req)) {
		return -1;
	}

	// set default values
	if (!uxslt.content_type_len) {
		if (!uxslt.content_type) {
			uxslt.content_type = "text/html";
		}
		uxslt.content_type_len = strlen(uxslt.content_type);
	}

	struct uwsgi_string_list *usl = uxslt.docroot;

	// first check for static docroots
	if (usl) {
		while(usl) {
			xmlfile = uwsgi_concat3n(usl->value, usl->len, "/", 1, wsgi_req->path_info, wsgi_req->path_info_len);
			if (uwsgi_is_file(xmlfile)) {
				break;
			}
			free(xmlfile);
			xmlfile = NULL;
			usl = usl->next;
		}	
	}
	// fallback to DOCUMENT_ROOT
	else {
		if (wsgi_req->document_root_len == 0) {
			uwsgi_403(wsgi_req);
			return UWSGI_OK;
		}

		xmlfile = uwsgi_concat3n(wsgi_req->document_root, wsgi_req->document_root_len, "/", 1, wsgi_req->path_info, wsgi_req->path_info_len);
	}

	if (!xmlfile) {
		uwsgi_404(wsgi_req);
		return UWSGI_OK;
	}

	// we have the full path, check if it is valid
	if (!uwsgi_expand_path(xmlfile, strlen(xmlfile), filename)) {
		free(xmlfile);
		uwsgi_404(wsgi_req);
		return UWSGI_OK;
	}

	free(xmlfile);

	if (!uwsgi_is_file(filename)) {
		uwsgi_403(wsgi_req);
		return UWSGI_OK;
	}
	filename_len = strlen(filename);

	// now search for the xslt file
	
	int found = 0;

	// first check for specific vars
	usl = uxslt.var;
	while(usl) {
		uint16_t rlen;
		char *value = uwsgi_get_var(wsgi_req, usl->value, usl->len, &rlen);
		if (value) {
			memcpy(stylesheet, value, rlen);
			stylesheet[rlen] = 0;
			stylesheet_len = rlen;
			found = 1;
			break;
		}
		usl = usl->next;
	}

	if (found) goto apply;

	// then check for custom extensions
	if (uxslt.ext) {
		usl = uxslt.ext;
		while(usl) {
			char *tmp_path = uwsgi_concat2n(filename, filename_len, usl->value, usl->len);
			if (uwsgi_is_file(tmp_path)) {
				stylesheet_len = filename_len + usl->len;
				memcpy(stylesheet, tmp_path, stylesheet_len);
				stylesheet[stylesheet_len] = 0;
				free(tmp_path);
				found = 1;
				break;
			}
			free(tmp_path);
			usl = usl->next;
		}
	}
	// use default extensions .xsl/.xslt
	else {
		char *tmp_path = uwsgi_concat2n(filename, filename_len, ".xsl", 4);
                if (uwsgi_is_file(tmp_path)) {
                	stylesheet_len = filename_len + 4;
                        memcpy(stylesheet, tmp_path, stylesheet_len);
			stylesheet[stylesheet_len] = 0;
                        free(tmp_path);
			goto apply;	
		}
                free(tmp_path);
		tmp_path = uwsgi_concat2n(filename, filename_len, ".xslt", 5);
                if (uwsgi_is_file(tmp_path)) {
                        stylesheet_len = filename_len + 5;
                        memcpy(stylesheet, tmp_path, stylesheet_len);
			stylesheet[stylesheet_len] = 0;
			found = 1;
                }
                free(tmp_path);
	}

	if (found) goto apply;

	// finally check for static stylesheets
	usl = uxslt.stylesheet;
	while(usl) {
		if (uwsgi_is_file(usl->value)) {
			memcpy(stylesheet, usl->value, usl->len);
			stylesheet_len = usl->len;
			stylesheet[stylesheet_len] = 0;
			found = 1;
			break;
		}
		usl = usl->next;
	}

	if (found) goto apply;
	
	uwsgi_404(wsgi_req);
	return UWSGI_OK;

apply:
	// we have both the file and the stylesheet, let's run the engine
	output = uwsgi_xslt_apply(filename, stylesheet, NULL, &output_rlen);
	if (!output) {
		uwsgi_500(wsgi_req);
		return UWSGI_OK;
	}

	// prepare headers
	if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) {
		uwsgi_500(wsgi_req);
		goto end;
	}
	// content_length
	if (uwsgi_response_add_content_length(wsgi_req, output_rlen)) {
		uwsgi_500(wsgi_req);
		goto end;
	}
	// content_type
	if (uwsgi_response_add_content_type(wsgi_req, uxslt.content_type, uxslt.content_type_len)) {
		uwsgi_500(wsgi_req);
		goto end;
	}

	uwsgi_response_write_body_do(wsgi_req, output, output_rlen);

end:
	xmlFree(output);
	return UWSGI_OK;
}

static void uwsgi_xslt_log(struct wsgi_request *wsgi_req) {
	log_request(wsgi_req);
}

struct uwsgi_plugin xslt_plugin = {
	.name = "xslt",
	.modifier1 = 23,
	.options = uwsgi_xslt_options,
	.request = uwsgi_request_xslt,
	.after_request = uwsgi_xslt_log,
};
