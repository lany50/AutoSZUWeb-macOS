#pragma once

#include "operation.h"
#include <curl/curl.h>

namespace Operation
{
    inline bool IsCancelled(const Cancelled& cancelled)
    {
        return cancelled && cancelled();
    }

    inline void ConfigureCancellation(CURL* curl, const Cancelled& cancelled)
    {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
            +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return IsCancelled(*static_cast<const Cancelled*>(context)) ? 1 : 0;
            });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
    }

    inline CURLcode Perform(CURL* curl, const Cancelled& cancelled)
    {
        if (IsCancelled(cancelled)) return CURLE_ABORTED_BY_CALLBACK;
        CURLM* multi = curl_multi_init();
        if (!multi) return CURLE_FAILED_INIT;
        if (curl_multi_add_handle(multi, curl) != CURLM_OK)
        {
            curl_multi_cleanup(multi);
            return CURLE_FAILED_INIT;
        }
        CURLcode result = CURLE_RECV_ERROR;
        int running = 1;
        while (running)
        {
            if (IsCancelled(cancelled)) { result = CURLE_ABORTED_BY_CALLBACK; break; }
            if (curl_multi_perform(multi, &running) != CURLM_OK) break;
            int queued = 0;
            while (CURLMsg* message = curl_multi_info_read(multi, &queued))
                if (message->msg == CURLMSG_DONE) result = message->data.result;
            if (running && curl_multi_poll(multi, nullptr, 0, 50, nullptr) != CURLM_OK) break;
        }
        curl_multi_remove_handle(multi, curl);
        curl_multi_cleanup(multi);
        return IsCancelled(cancelled) ? CURLE_ABORTED_BY_CALLBACK : result;
    }
}
