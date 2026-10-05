/*
 * geistr_engine.h — for processes that hold the geistlib engine themselves
 * (geist-serve's geistd keeps its token-level protocol on the same model).
 * Most users want geistr_model_open instead. EXPERIMENTAL.
 */
#pragma once
#include "geistr.h"

#ifdef __cplusplus
extern "C" {
#endif

struct geist_model;
struct geist_backend;

/* A geistr model over an engine model the caller already loaded, so it is in
 * memory once. Borrowed: the caller keeps model and backend alive until the
 * last chat on it is closed, and destroys them itself. The context window is
 * the model's trained one, or opts->context if smaller; processor and
 * threads are the caller's choice already made. Chat calls serialize engine
 * use on GPU backends; engine calls the caller makes itself on the same
 * model must not run at the same time as a chat call. */
geistr_status geistr_model_wrap(struct geist_model     *model,
                                struct geist_backend   *backend,
                                const geistr_model_opts *opts,
                                geistr_model           **out,
                                char                    *error,
                                size_t                   error_cap);

#ifdef __cplusplus
}
#endif
