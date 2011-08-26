#include "geocache.h"
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <apr_getopt.h>


typedef struct geocache_context_seeding geocache_context_seeding;
struct geocache_context_seeding{
    geocache_context ctx;
    int (*get_next_tile)(geocache_context_seeding *ctx, geocache_tile *tile, geocache_context *tmpctx);
    apr_thread_mutex_t *mutex;
    geocache_tileset *tileset;
    int minzoom;
    int maxzoom;
    double *extent;
    int nextx,nexty,nextz;
};

typedef struct {
    int firstx;
    int firsty;
    int lastx;
    int lasty;
} gc_tiles_for_zoom;

gc_tiles_for_zoom seed_tiles[100]; //TODO: bug here if more than 100 zoomlevels, or if multithreaded usage on multiple tilesets

static const apr_getopt_option_t seed_options[] = {
    /* long-option, short-option, has-arg flag, description */
    { "config", 'c', TRUE, "configuration file"},
    { "tileset", 't', TRUE, "tileset to seed" },
    { "zoom", 'z', TRUE, "min and max zoomlevels to seed" },
    { "extent", 'e', TRUE, "extent" },
    { "nthreads", 'n', TRUE, "number of parallel threads to use" },
    { "help", 'h', FALSE, "show help" },    
    { NULL, 0, 0, NULL },
};

void geocache_context_seeding_lock_aquire(geocache_context *gctx, int blocking) {
    int ret;
    geocache_context_seeding *ctx = (geocache_context_seeding*)gctx;
    ret = apr_thread_mutex_lock(ctx->mutex);
    if(ret != APR_SUCCESS) {
        gctx->set_error(gctx,GEOCACHE_MUTEX_ERROR, "failed to lock mutex");
        return;
    }
    apr_pool_cleanup_register(gctx->pool, ctx->mutex, (void*)apr_thread_mutex_unlock, apr_pool_cleanup_null);
}

void geocache_context_seeding_lock_release(geocache_context *gctx) {
    int ret;
    geocache_context_seeding *ctx = (geocache_context_seeding*)gctx;
    ret = apr_thread_mutex_unlock(ctx->mutex);
    if(ret != APR_SUCCESS) {
        gctx->set_error(gctx,GEOCACHE_MUTEX_ERROR, "failed to unlock mutex");
        return;
    }
    apr_pool_cleanup_kill(gctx->pool, ctx->mutex, (void*)apr_thread_mutex_unlock);
}

void geocache_context_seeding_log(geocache_context *ctx, geocache_log_level level, char *msg, ...) {
    va_list args;
    va_start(args,msg);
    vfprintf(stderr,msg,args);
    va_end(args);
}

int tile_exists(geocache_context *ctx, geocache_tileset *tileset,
                    int x, int y, int z,
                    geocache_context *tmpctx) {
    geocache_tile tile;
    tile.x = x;
    tile.y = y;
    tile.z = z;
    tile.tileset = tileset;
    return tileset->cache->tile_exists(tmpctx,&tile);
}

int geocache_context_seeding_get_next_tile(geocache_context_seeding *ctx, geocache_tile *tile, geocache_context *tmpcontext) {
    geocache_context *gctx= (geocache_context*)ctx;

    gctx->global_lock_aquire(gctx,0);
    if(ctx->nextz > ctx->maxzoom) {
        gctx->global_lock_release(gctx);
        return GEOCACHE_FAILURE;
        //we have no tiles left to process
    }
    
    tile->x = ctx->nextx;
    tile->y = ctx->nexty;
    tile->z = ctx->nextz;



    while(1) {
        ctx->nextx += ctx->tileset->metasize_x;
        if(ctx->nextx > seed_tiles[tile->z].lastx) {
            ctx->nexty += ctx->tileset->metasize_y;
            if(ctx->nexty > seed_tiles[tile->z].lasty) {
                ctx->nextz += 1;
                if(ctx->nextz > ctx->maxzoom) break;
                ctx->nexty = seed_tiles[ctx->nextz].firsty;
            }
            ctx->nextx = seed_tiles[ctx->nextz].firstx;
        }
        if(! tile_exists(gctx, ctx->tileset, ctx->nextx, ctx->nexty, ctx->nextz,tmpcontext))
            break;
    }
    gctx->global_lock_release(gctx);
    return GEOCACHE_SUCCESS;
}

void geocache_context_seeding_init(geocache_context_seeding *ctx,
        geocache_cfg *cfg,
        geocache_tileset *tileset,
        int minzoom, int maxzoom,
        double *extent) {
    int ret;
    geocache_context *gctx = (geocache_context*)ctx;
    geocache_context_init(gctx);
    ret = apr_thread_mutex_create(&ctx->mutex,APR_THREAD_MUTEX_DEFAULT,gctx->pool);
    if(ret != APR_SUCCESS) {
        gctx->set_error(gctx,GEOCACHE_MUTEX_ERROR,"failed to create mutex");
        return;
    }
    gctx->global_lock_aquire = geocache_context_seeding_lock_aquire;
    gctx->global_lock_release = geocache_context_seeding_lock_release;
    gctx->log = geocache_context_seeding_log;
    ctx->get_next_tile = geocache_context_seeding_get_next_tile;
    ctx->extent = extent;
    ctx->minzoom = minzoom;
    ctx->maxzoom = maxzoom;
    ctx->tileset = tileset;
}

void dummy_lock_aquire(geocache_context *ctx, int nonblocking){

}

void dummy_lock_release(geocache_context *ctx) {

}

void dummy_log(geocache_context *ctx, geocache_log_level level, char *msg, ...) {

}

static void* APR_THREAD_FUNC doseed(apr_thread_t *thread, void *data) {
    geocache_context_seeding *ctx = (geocache_context_seeding*)data;
    geocache_context *gctx = (geocache_context*)ctx;
    geocache_tile *tile = geocache_tileset_tile_create(gctx->pool, ctx->tileset);
    geocache_context tile_ctx;
    geocache_context_init(&tile_ctx);
    tile_ctx.global_lock_aquire = dummy_lock_aquire;
    tile_ctx.global_lock_release = dummy_lock_release;
    tile_ctx.log = geocache_context_seeding_log;
    apr_pool_create(&tile_ctx.pool,NULL);
    while(GEOCACHE_SUCCESS == ctx->get_next_tile(ctx,tile,&tile_ctx)) {
        geocache_tileset_tile_get(&tile_ctx,tile);
        if(tile_ctx.get_error(&tile_ctx)) {
            gctx->set_error(gctx,tile_ctx.get_error(&tile_ctx),tile_ctx.get_error_message(&tile_ctx));
            apr_thread_exit(thread,GEOCACHE_FAILURE);
        }
        apr_pool_clear(tile_ctx.pool);
    }

    if(gctx->get_error(gctx)) {
        apr_thread_exit(thread,GEOCACHE_FAILURE);
    }
    
    apr_thread_exit(thread,GEOCACHE_SUCCESS);
    return NULL;
}


int usage(const char *progname, char *msg) {
    printf("%s\nusage: %s options\n"
            "-c|--config conffile : configuration file to load\n"
            "-t|--tileset tileset : name of the tileset to seed\n"
            "[-z|--zoom minzoom,maxzoom] : zoomlevels to seed\n"
            "[-e|--extent minx,miny,maxx,maxy] : extent to seed\n"
            "[-n|--nthreads n] : number of parallel threads\n",
            msg,progname);
    return 1;
}

int main(int argc, const char **argv) {
    /* initialize apr_getopt_t */
    apr_getopt_t *opt;
    const char *configfile=NULL;
    geocache_cfg *cfg = NULL;
    geocache_tileset *tileset;
    geocache_context_seeding ctx;
    geocache_context *gctx = (geocache_context*)&ctx;
    apr_thread_t **threads;
    apr_threadattr_t *thread_attrs;
    const char *tileset_name=NULL;
    int *zooms = NULL;//[2];
    double *extent = NULL;//[4];
    int nthreads=1;
    int optch;
    int rv,n;
    const char *optarg;
    apr_initialize();
    apr_pool_create_core(&gctx->pool);
    geocache_context_init(gctx);
    cfg = geocache_configuration_create(gctx->pool);
    apr_getopt_init(&opt, gctx->pool, argc, argv);
    /* parse the all options based on opt_option[] */
    while ((rv = apr_getopt_long(opt, seed_options, &optch, &optarg)) == APR_SUCCESS) {
        switch (optch) {
            case 'h':
                return usage(argv[0],NULL);
                break;
            case 'c':
                configfile = optarg;
                break;
            case 't':
                tileset_name = optarg;
                break;
            case 'n':
                nthreads = (int)strtol(optarg, NULL, 10);
                break;
            case 'e':
                if ( GEOCACHE_SUCCESS != geocache_util_extract_double_list(gctx, (char*)optarg, ',', &extent, &n) ||
                        n != 4 || extent[0] >= extent[2] || extent[1] >= extent[3] ) {
                    return usage(argv[0], "failed to parse extent, expecting comma separated 4 doubles");
                }
                break;
            case 'z':
                if ( GEOCACHE_SUCCESS != geocache_util_extract_int_list(gctx, (char*)optarg, ',', &zooms, &n) ||
                        n != 2 || zooms[0] >= zooms[1]) {
                    return usage(argv[0], "failed to parse zooms, expecting comma separated 2 ints");
                }
                break;
        }
    }
    if (rv != APR_EOF) {
        return usage(argv[0],"bad options");
    }

    if( ! configfile ) {
        return usage(argv[0],"config not specified");
    } else {
        geocache_configuration_parse(gctx,configfile,cfg);
        if(gctx->get_error(gctx))
            return usage(argv[0],gctx->get_error_message(gctx));
    }

    if( ! tileset_name ) {
        return usage(argv[0],"tileset not specified");
    } else {
        tileset = geocache_configuration_get_tileset(cfg,tileset_name);
        if(!tileset) {
            return usage(argv[0], "tileset not found in configuration");
        }
        if(!zooms) {
            zooms = (int*)apr_pcalloc(gctx->pool,2*sizeof(int));
            zooms[0] = 1;
            zooms[1] = tileset->levels;
        }
        if(!extent) {
            extent = (double*)apr_pcalloc(gctx->pool,4*sizeof(double));
            extent[0] = tileset->extent[0];
            extent[1] = tileset->extent[1];
            extent[2] = tileset->extent[2];
            extent[3] = tileset->extent[3];
        }
    }

    geocache_context_seeding_init(&ctx,cfg,tileset,zooms[0],zooms[1],extent);
    for(n=zooms[0];n<=zooms[1];n++) {
        geocache_tileset_get_xy(gctx,tileset,tileset->extent[0],tileset->extent[1],
                n,&seed_tiles[n].firstx,&seed_tiles[n].firsty);
        geocache_tileset_get_xy(gctx,tileset,tileset->extent[2],tileset->extent[3],
                n,&seed_tiles[n].lastx,&seed_tiles[n].lasty);
    }
    ctx.nextz = zooms[0];
    ctx.nextx = seed_tiles[zooms[0]].firstx;
    ctx.nexty = seed_tiles[zooms[0]].firsty;

    if( ! nthreads ) {
        return usage(argv[0],"failed to parse nthreads, must be int");
    } else {
        apr_threadattr_create(&thread_attrs, gctx->pool);
        threads = (apr_thread_t**)apr_pcalloc(gctx->pool, nthreads*sizeof(apr_thread_t*));
        for(n=0;n<nthreads;n++) {
            apr_thread_create(&threads[n], thread_attrs, doseed, (void*)&ctx, gctx->pool);
        }
        for(n=0;n<nthreads;n++) {
            apr_thread_join(&rv, threads[n]);
        }
    }
    if(gctx->get_error(gctx)) {
        gctx->log(gctx,GEOCACHE_ERROR,gctx->get_error_message(gctx));
    }


    return 0;
}