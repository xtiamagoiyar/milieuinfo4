/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdio.h>

#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include <nghttp2/nghttp2.h>

#include "h2_private.h"
#include "h2_bucket.h"
#include "h2_util.h"
#include "h2_response.h"

#define H2_CREATE_NV_LIT_CS(nv, NAME, VALUE) nv->name = (uint8_t *)NAME;      \
                                             nv->namelen = sizeof(NAME) - 1;  \
                                             nv->value = (uint8_t *)VALUE;    \
                                             nv->valuelen = strlen(VALUE)

#define H2_CREATE_NV_CS_LIT(nv, NAME, VALUE) nv->name = (uint8_t *)NAME;      \
                                             nv->namelen = strlen(NAME);      \
                                             nv->value = (uint8_t *)VALUE;    \
                                             nv->valuelen = sizeof(VALUE) - 1

#define H2_CREATE_NV_CS_CS(nv, NAME, VALUE) nv->name = (uint8_t *)NAME;       \
                                            nv->namelen = strlen(NAME);       \
                                            nv->value = (uint8_t *)VALUE;     \
                                            nv->valuelen = strlen(VALUE)

h2_response *h2_response_create(int stream_id,
                                  apr_status_t task_status,
                                  const char *http_status,
                                  apr_array_header_t *hlines,
                                  h2_bucket *data,
                                  apr_pool_t *pool)
{
    apr_size_t nvlen = 1 + (hlines? hlines->nelts : 0);
    /* we allocate one block for the h2_response and the array of
     * nghtt2_nv structures.
     */
    h2_response *head = calloc(1, sizeof(h2_response)
                                + (nvlen * sizeof(nghttp2_nv)));
    if (head == NULL) {
        return NULL;
    }

    head->stream_id = stream_id;
    head->task_status = task_status;
    head->http_status = http_status;
    head->data = data;
    head->content_length = -1;

    if (hlines) {
        nghttp2_nv *nvs = (nghttp2_nv *)&head->nv;
        H2_CREATE_NV_LIT_CS(nvs, ":status", http_status);

        int seen_clen = 0;
        for (int i = 0; i < hlines->nelts; ++i) {
            char *hline = ((char **)hlines->elts)[i];
            nghttp2_nv *nv = &nvs[i + 1];
            char *sep = strchr(hline, ':');
            if (!sep) {
                /* not valid format, abort */
                return NULL;
            }
            (*sep++) = '\0';
            while (*sep == ' ' || *sep == '\t') {
                ++sep;
            }
            if (*sep) {
                const char *hname = h2_strlwr(hline);
                if (!strcmp("transfer-encoding", hname)) {
                    /* never forward this header */
                    if (!strcmp("chunked", sep)) {
                        head->chunked = 1;
                    }
                }
                else {
                    H2_CREATE_NV_CS_CS(nv, hname, sep);
                }
            }
            else {
                /* reached end of line, an empty header value */
                H2_CREATE_NV_CS_LIT(nv, h2_strlwr(hline), "");
            }
        }
        head->nvlen = nvlen;

        for (int i = 1; i < head->nvlen; ++i) {
            const nghttp2_nv *nv = &(&head->nv)[i];
            if (!head->chunked && !strcmp("content-length", (char*)nv->name)) {
                apr_int64_t clen = apr_atoi64((char*)nv->value);
                if (clen <= 0) {
                    ap_log_perror(APLOG_MARK, APLOG_WARNING, APR_EINVAL, pool,
                                  "h2_response(%d): content-length value not parsed: %s",
                                  head->stream_id, (char*)nv->value);
                    return NULL;
                }
                head->content_length = clen;
            }
        }
    }
    return head;
}

void h2_response_destroy(h2_response *head)
{
    if (head->data) {
        h2_bucket_destroy(head->data);
        head->data = NULL;
    }
    free(head);
}

long h2_response_get_content_length(h2_response *resp)
{
    return resp->content_length;
}
