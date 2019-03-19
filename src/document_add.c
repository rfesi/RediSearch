#include "document.h"
#include "err.h"
#include "util/logging.h"
#include "commands.h"

/*
## FT.ADD <index> <docId> <score> [NOSAVE] [REPLACE] [PARTIAL] [IF <expr>] [LANGUAGE <lang>]
[PAYLOAD {payload}] FIELDS <field> <text> ....] Add a documet to the index.

## Parameters:

    - index: The Fulltext index name. The index must be first created with
FT.CREATE

    - docId: The document's id that will be returned from searches. Note that
the same docId cannot
be
    added twice to the same index

    - score: The document's rank based on the user's ranking. This must be
between 0.0 and 1.0.
    If you don't have a score just set it to 1

    - NOSAVE: If set to true, we will not save the actual document in the index
and only index it.

    - REPLACE: If set, we will do an update and delete an older version of the document if it
exists

    - FIELDS: Following the FIELDS specifier, we are looking for pairs of
<field> <text> to be
indexed.
    Each field will be scored based on the index spec given in FT.CREATE.
    Passing fields that are not in the index spec will make them be stored as
part of the document,
    or ignored if NOSAVE is set

    - LANGUAGE lang: If set, we use a stemmer for the supplied langauge during
indexing. Defaults to
English.
   If an unsupported language is sent, the command returns an error.
   The supported languages are:

   > "arabic",  "danish",    "dutch",   "english",   "finnish",    "french",
   > "german",  "hungarian", "italian", "norwegian", "portuguese", "romanian",
   > "russian", "spanish",   "swedish", "tamil",     "turkish"


Returns OK on success, NOADD if the document was not added due to an IF expression not evaluating to
true or an error if something went wrong.
*/

static int parseDocumentOptions(AddDocumentOptions *opts, ArgsCursor *ac, QueryError *status) {
  // Assume argc and argv are at proper indices
  int nosave = 0, replace = 0, partial = 0, foundFields = 0;
  opts->fieldsArray = NULL;
  opts->numFieldElems = 0;
  opts->options = 0;

  ACArgSpec argList[] = {{.name = "NOSAVE", .type = AC_ARGTYPE_BOOLFLAG, .target = &nosave},
                         {.name = "REPLACE", .type = AC_ARGTYPE_BOOLFLAG, .target = &replace},
                         {.name = "PARTIAL", .type = AC_ARGTYPE_BOOLFLAG, .target = &partial},
                         {.name = "PAYLOAD", .type = AC_ARGTYPE_RSTRING, .target = &opts->payload},
                         {.name = "LANGUAGE", .type = AC_ARGTYPE_STRING, .target = &opts->language},
                         {.name = "IF", .type = AC_ARGTYPE_STRING, .target = &opts->evalExpr},
                         {.name = NULL}};

  while (!AC_IsAtEnd(ac)) {
    int rv = 0;
    ACArgSpec *errArg = NULL;

    if ((rv = AC_ParseArgSpec(ac, argList, &errArg)) == AC_OK) {
      continue;
    } else if (rv == AC_ERR_ENOENT) {
      size_t narg;
      const char *s = AC_GetStringNC(ac, &narg);
      if (!strncasecmp("FIELDS", s, narg)) {
        size_t numRemaining = AC_NumRemaining(ac);
        if (numRemaining % 2 != 0) {
          QueryError_SetError(status, QUERY_EADDARGS,
                              "Fields must be specified in FIELD VALUE pairs");
          return REDISMODULE_ERR;
        } else {
          opts->fieldsArray = (RedisModuleString **)ac->objs + ac->offset;
          opts->numFieldElems = numRemaining;
          foundFields = 1;
        }
        break;

      } else {
        const char *unknown = AC_GetStringNC(ac, NULL);
        QueryError_SetErrorFmt(status, QUERY_EADDARGS, "Unknown keyword `%.*s` provided", (int)narg,
                               unknown);
      }
      // Argument not found, that's ok. We'll handle it below
    } else {
      QueryError_SetErrorFmt(status, QUERY_EADDARGS, "%s: %s", errArg->name, AC_Strerror(rv));
      return REDISMODULE_ERR;
    }
  }

  if (!foundFields) {
    // If we've reached here, there is no fields list. This is an error??
    QueryError_SetError(status, QUERY_EADDARGS, "No field list found");
    return REDISMODULE_ERR;
  }

  if (opts->language && !IsSupportedLanguage(opts->language, strlen(opts->language))) {
    QueryError_SetError(status, QUERY_EADDARGS, "Unsupported language");
    return REDISMODULE_ERR;
  }

  if (QueryError_HasError(status)) {
    return REDISMODULE_ERR;
  }
  if (partial) {
    opts->options |= DOCUMENT_ADD_PARTIAL;
  }
  if (nosave) {
    opts->options |= DOCUMENT_ADD_NOSAVE;
  }
  if (replace) {
    opts->options |= DOCUMENT_ADD_REPLACE;
  }
  return REDISMODULE_OK;
}

int RS_AddDocument(RedisSearchCtx *sctx, RedisModuleString *name, const AddDocumentOptions *opts,
                   QueryError *status) {
  int rc = REDISMODULE_ERR;
  // If the ID is 0, then the document does not exist.
  IndexSpec *sp = sctx->spec;
  int exists = !!DocTable_GetIdR(&sp->docs, name);
  if (exists && !(opts->options & DOCUMENT_ADD_REPLACE)) {
    QueryError_SetError(status, QUERY_EDOCEXISTS, NULL);
    goto error;
  }

  // handle update condition, only if the document exists
  if (exists && opts->evalExpr) {
    int res = 0;
    if (Document_EvalExpression(sctx, name, opts->evalExpr, &res, status) == REDISMODULE_OK) {
      if (res == 0) {
        QueryError_SetError(status, QUERY_EDOCNOTADDED, NULL);
        goto error;
      }
    } else {
      printf("Eval failed! (%s)\n", opts->evalExpr);
      if (status->code == QUERY_ENOPROPVAL) {
        QueryError_ClearError(status);
        QueryError_SetCode(status, QUERY_EDOCNOTADDED);
      }
      goto error;
    }
  }

  RedisModuleCtx *ctx = sctx->redisCtx;
  Document doc = {0};

  Document_PrepareForAdd(&doc, name, opts->score, opts, sctx->redisCtx);
  if (!(opts->options & DOCUMENT_ADD_NOSAVE)) {
    RedisSearchCtx sctx_s = SEARCH_CTX_STATIC(sctx->redisCtx, sp);
    if (Redis_SaveDocument(&sctx_s, &doc, status) != REDISMODULE_OK) {
      Document_FreeDetached(&doc, ctx);
    }
  }

  LG_DEBUG("Adding doc %s with %d fields\n", RedisModule_StringPtrLen(doc.docKey, NULL),
           doc.numFields);
  RSAddDocumentCtx *aCtx = NewAddDocumentCtx(sp, &doc, status);
  if (aCtx == NULL) {
    Document_FreeDetached(&doc, ctx);
    goto error;
  }

  uint32_t addOptions = opts->options;

  if (!exists) {
    // If the document does not exist, remove replace/partial settings
    addOptions &= ~(DOCUMENT_ADD_REPLACE | DOCUMENT_ADD_PARTIAL);
  }
  if (addOptions & DOCUMENT_ADD_CURTHREAD) {
    aCtx->stateFlags |= ACTX_F_NOBLOCK;
  }

  aCtx->donecb = opts->donecb;
  AddDocumentCtx_Submit(aCtx, sctx, addOptions);
  return REDISMODULE_OK;

error:
  return REDISMODULE_ERR;
}

static void replyCallback(RSAddDocumentCtx *aCtx, RedisModuleCtx *ctx, void *unused) {
  if (QueryError_HasError(&aCtx->status)) {
    if (aCtx->status.code == QUERY_EDOCNOTADDED) {
      RedisModule_ReplyWithError(ctx, "NOADD");
    } else {
      RedisModule_ReplyWithError(ctx, QueryError_GetError(&aCtx->status));
    }
  } else {
    RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
}

static int doAddDocument(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int canBlock) {
  if (argc < 4) {
    // cmd, index, document, [arg] ...
    return RedisModule_WrongArity(ctx);
  }

  ArgsCursor ac;
  AddDocumentOptions opts = {.donecb = replyCallback};
  QueryError status = {0};

  ArgsCursor_InitRString(&ac, argv + 3, argc - 3);

  int rv = 0;
  if ((rv = AC_GetDouble(&ac, &opts.score, 0) != AC_OK)) {
    QueryError_SetError(&status, QUERY_EADDARGS, "Could not parse document score");
  } else if (opts.score < 0 || opts.score > 1.0) {
    QueryError_SetError(&status, QUERY_EADDARGS, "Score must be between 0 and 1");
  } else if (parseDocumentOptions(&opts, &ac, &status) != REDISMODULE_OK) {
    QueryError_MaybeSetCode(&status, QUERY_EADDARGS);
  }

  if (QueryError_HasError(&status)) {
    if (status.code == QUERY_EDOCNOTADDED) {
      RedisModule_ReplyWithSimpleString(ctx, "NOADD");
    } else {
      RedisModule_ReplyWithError(ctx, QueryError_GetError(&status));
    }
    goto cleanup;
  }

  if (canBlock) {
    canBlock = CheckConcurrentSupport(ctx);
  }

  RedisModule_AutoMemory(ctx);
  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 0);
  if (!sp) {
    RedisModule_ReplyWithError(ctx, "Unknown index name");
    goto cleanup;
  }
  if (canBlock) {
    canBlock = !(sp->flags & Index_Temporary) && CheckConcurrentSupport(ctx);
  }

  if (!canBlock) {
    opts.options |= DOCUMENT_ADD_CURTHREAD;
  }
  RedisSearchCtx sctx = {.redisCtx = ctx, .spec = sp};
  rv = RS_AddDocument(&sctx, argv[2], &opts, &status);
  if (rv != REDISMODULE_OK) {
    if (status.code == QUERY_EDOCNOTADDED) {
      RedisModule_ReplyWithSimpleString(ctx, "NOADD");
    } else {
      RedisModule_ReplyWithError(ctx, QueryError_GetError(&status));
    }
  } else {
    // Replicate *here*
    RedisModule_Replicate(ctx, RS_SAFEADD_CMD, "v", argv + 1, argc - 1);
  }

cleanup:
  QueryError_ClearError(&status);
  return REDISMODULE_OK;
}

/* FT.ADDHASH <index> <docId> <score> [LANGUAGE <lang>] [REPLACE]
*  Index a document that's already saved in redis as a HASH object, unrelated to
* the module.
*  This will not modify the document, just add it to the index if it is not
* already there.

## Parameters:

      - index: The Fulltext index name. The index must be first created with
        FT.CREATE

      - docId: The document's id, that must be a HASH key already in redis.

      - score: The document's rank based on the user's ranking. This must be
        between 0.0 and 1.0.
        If you don't have a score just set it to 1

      - REPLACE: If set, we will do an update and delete an older version of the document if it
exists

      - LANGUAGE lang: If set, we use a stemmer for the supplied langauge during
      indexing. Defaults to
      English.
        If an unsupported language is sent, the command returns an error.
        The supported languages are:

        > "arabic",  "danish",    "dutch",   "english",   "finnish", "french",
        > "german",  "hungarian", "italian", "norwegian", "portuguese",
"romanian",
        > "russian", "spanish",   "swedish", "tamil",     "turkish"


  Returns OK on success, or an error if something went wrong.
*/
static int doAddHashCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                            int isBlockable) {
  if (argc < 4 || argc > 7) {
    return RedisModule_WrongArity(ctx);
  }

  RedisModule_AutoMemory(ctx);

  QueryError status = {0};
  ArgsCursor ac = {0};
  ArgsCursor_InitRString(&ac, argv + 3, argc - 3);
  double ds;
  int rv = 0;

  if ((rv = AC_GetDouble(&ac, &ds, 0)) != AC_OK) {
    QueryError_SetError(&status, QUERY_EADDARGS, "Could not parse document score");
    goto cleanup;
  } else if (ds < 0 || ds > 1.0) {
    QueryError_SetError(&status, QUERY_EADDARGS, "Score must be between 0 and 1");
    goto cleanup;
  }

  int replace = 0;
  const char *language = NULL;
  ACArgSpec specs[] =  // Comment to force newline
      {{.name = "LANGUAGE", .type = AC_ARGTYPE_STRING, .target = &language},
       {.name = "REPLACE", .type = AC_ARGTYPE_BOOLFLAG, .target = &replace},
       {.name = NULL}};
  ACArgSpec *errArg = NULL;
  rv = AC_ParseArgSpec(&ac, specs, &errArg);
  if (rv == AC_OK) {
    // OK. No error
  } else if (rv == AC_ERR_ENOENT) {
    QueryError_SetErrorFmt(&status, QUERY_EADDARGS, "Unknown keyword: `%s`",
                           AC_GetStringNC(&ac, NULL));
  } else {
    QueryError_SetErrorFmt(&status, QUERY_EADDARGS, "Error parsing arguments for `%s`: %s",
                           errArg ? errArg->name : "", AC_Strerror(rv));
  }

  if (language && !IsSupportedLanguage(language, strlen(language))) {
    QueryError_SetErrorFmt(&status, QUERY_EADDARGS, "Unknown language: `%s`", language);
    goto cleanup;
  }

  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 1);
  if (sp == NULL) {
    QueryError_SetErrorFmt(&status, QUERY_EGENERIC, "Unknown Index name");
    goto cleanup;
  }

  // Load the document score

  Document doc;
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(ctx, sp);
  if (Redis_LoadDocument(&sctx, argv[2], &doc) != REDISMODULE_OK) {
    return RedisModule_ReplyWithError(ctx, "Could not load document");
  }

  doc.docKey = argv[2];
  doc.score = ds;
  doc.language = language ? language : DEFAULT_LANGUAGE;
  doc.payload = NULL;
  doc.payloadSize = 0;
  Document_Detach(&doc, ctx);

  LG_DEBUG("Adding doc %s with %d fields\n", RedisModule_StringPtrLen(doc.docKey, NULL),
           doc.numFields);

  RSAddDocumentCtx *aCtx = NewAddDocumentCtx(sp, &doc, &status);
  if (aCtx == NULL) {
    Document_FreeDetached(&doc, ctx);
    return QueryError_ReplyAndClear(ctx, &status);
  }

  aCtx->donecb = replyCallback;

  if (isBlockable) {
    isBlockable = CheckConcurrentSupport(ctx);
  }

  if (!isBlockable) {
    aCtx->stateFlags |= ACTX_F_NOBLOCK;
  }

  RedisModule_Replicate(ctx, RS_SAFEADDHASH_CMD, "v", argv + 1, argc - 1);
  AddDocumentCtx_Submit(aCtx, &sctx, replace ? DOCUMENT_ADD_REPLACE : 0);
  return REDISMODULE_OK;

cleanup:
  assert(QueryError_HasError(&status));
  RedisModule_ReplyWithError(ctx, QueryError_GetError(&status));
  QueryError_ClearError(&status);
  return REDISMODULE_OK;
}

int RSAddDocumentCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return doAddDocument(ctx, argv, argc, 1);
}
int RSSafeAddDocumentCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return doAddDocument(ctx, argv, argc, 0);
}

int RSAddHashCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return doAddHashCommand(ctx, argv, argc, 1);
}

int RSSafeAddHashCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return doAddHashCommand(ctx, argv, argc, 0);
}