/*
 * olamaps.h - Ola Maps REST client (place search, reverse geocoding, directions,
 * distance matrix), using libcurl for HTTPS.
 *
 * Endpoints (https://api.olamaps.io), authenticated with the api_key query parameter:
 *     GET  /places/v1/autocomplete       input, location, radius
 *     GET  /places/v1/details            place_id
 *     GET  /places/v1/reverse-geocode    latlng
 *     POST /routing/v1/directions        origin, destination  ("lat,lng")
 *     GET  /routing/v1/distanceMatrix    origins, destinations ("lat,lng|lat,lng")
 *
 * Every answer is validated: anything malformed, negative, NaN or infinite becomes
 * an error instead of a crash. The key is never written to a log.
 */
#ifndef OLAMAPS_H
#define OLAMAPS_H

#include <curl/curl.h>
#include <pthread.h>

#include "cache.h"

typedef enum {
    OLA_OK = 0,
    OLA_ERROR,          /* could not be reached or answered something unusable -> HTTP 502 */
    OLA_NOT_FOUND,      /* the place or route does not exist                    -> HTTP 404 */
    OLA_REJECTED        /* Ola Maps refused this particular input               -> HTTP 422 */
} OlaStatus;

typedef struct {
    OlaStatus status;
    char message[200];
} OlaError;

typedef struct {
    char *api_key;
    char *base_url;
    TTLCache *place_cache, *route_cache, *miss_cache;
    pthread_mutex_t count_lock;
    long calls_total, calls_today, count_day;
    CURLSH *share;
    pthread_mutex_t share_locks[CURL_LOCK_DATA_LAST];
} OlaClient;

typedef struct {
    int ok;
    double km, min;
} RoadCell;

typedef struct {
    double km, min;
    char *polyline;              /* may be NULL */
} Road;

#define CITY_CENTRE_LAT 12.9716
#define CITY_CENTRE_LNG 77.5946

OlaClient *ola_new(const char *api_key, const char *base_url);
void ola_free(OlaClient *c);
void ola_usage(OlaClient *c, long *today, long *total);

char *clean_query(const char *text);
/* All return 0 on success, -1 with err filled in. */
int ola_autocomplete(OlaClient *c, const char *text, char **json_out, OlaError *err);
int ola_place_location(OlaClient *c, const char *place_id, double *lat, double *lng, OlaError *err);
int ola_reverse(OlaClient *c, double lat, double lng, char **name, char **address, OlaError *err);
int ola_directions(OlaClient *c, double olat, double olng, double dlat, double dlng, Road *out, OlaError *err);
int ola_distance_matrix(OlaClient *c, const double (*origins)[2], int no, const double (*dests)[2], int nd,
                        RoadCell *out, OlaError *err);

#endif
