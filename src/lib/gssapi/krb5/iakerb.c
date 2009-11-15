/* -*- mode: c; indent-tabs-mode: nil -*- */
/*
 * Copyright 2009  by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 */
#include "k5-int.h"
#include "gssapiP_krb5.h"
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#include <assert.h>

/*
 * IAKERB implementation
 */

struct _iakerb_ctx_id_rec {
    krb5_magic magic;
    krb5_context k5c;
    krb5_init_creds_context icc;
    krb5_data conv;
    gss_ctx_id_t gssc;
    unsigned int complete;
};

typedef struct _iakerb_ctx_id_rec iakerb_ctx_id_rec;
typedef iakerb_ctx_id_rec *iakerb_ctx_id_t;

static void
iakerb_release_context(iakerb_ctx_id_t ctx)
{
    OM_uint32 tmp;

    if (ctx == NULL)
        return;

    krb5_gss_delete_sec_context(&tmp, &ctx->gssc, NULL);
    krb5_init_creds_free(ctx->k5c, ctx->icc);
    krb5_free_data_contents(ctx->k5c, &ctx->conv);
    krb5_free_context(ctx->k5c);
    free(ctx);
}

krb5_error_code
iakerb_make_finished(krb5_context context,
                     krb5_key key,
                     const krb5_data *conv,
                     krb5_data **finished)
{
    krb5_error_code code;
    krb5_cksumtype cksumtype;
    krb5_iakerb_finished iaf;

    *finished = NULL;

    memset(&iaf, 0, sizeof(iaf));

    if (key == NULL)
        return KRB5KDC_ERR_NULL_KEY;

    code = krb5int_c_mandatory_cksumtype(context,
                                         krb5_k_key_enctype(context, key),
                                         &cksumtype);
    if (code != 0)
        return code;

    code = krb5_k_make_checksum(context, cksumtype,
                                key, KRB5_KEYUSAGE_IAKERB_FINISHED,
                                conv, &iaf.checksum);
    if (code != 0)
        return code;

    code = encode_krb5_iakerb_finished(&iaf, finished);

    krb5_free_checksum_contents(context, &iaf.checksum);

    return code;
}

krb5_error_code
iakerb_verify_finished(krb5_context context,
                       krb5_key key,
                       const krb5_data *conv,
                       const krb5_data *finished)
{
    krb5_error_code code;
    krb5_iakerb_finished *iaf;
    krb5_boolean valid = FALSE;

    if (key == NULL)
        return KRB5KDC_ERR_NULL_KEY;

    code = decode_krb5_iakerb_finished(finished, &iaf);
    if (code != 0)
        return code;

    code = krb5_k_verify_checksum(context, key, KRB5_KEYUSAGE_IAKERB_FINISHED,
                                  conv, &iaf->checksum, &valid);
    if (code == 0 && valid == FALSE)
        code = KRB5KRB_AP_ERR_BAD_INTEGRITY;

    krb5_free_iakerb_finished(context, iaf);

    return code;
}

static krb5_error_code
iakerb_save_token(iakerb_ctx_id_t ctx, const gss_buffer_t token)
{
    char *p;

    p = realloc(ctx->conv.data, ctx->conv.length + token->length);
    if (p == NULL)
        return ENOMEM;

    memcpy(p + ctx->conv.length, token->value, token->length);
    ctx->conv.data = p;
    ctx->conv.length += token->length;

    return 0;
}

static krb5_error_code
iakerb_parse_token(iakerb_ctx_id_t ctx,
                   int initialContextToken,
                   const gss_buffer_t token,
                   krb5_data *realm,
                   krb5_data **cookie,
                   krb5_data *request)
{
    krb5_error_code code;
    krb5_iakerb_header *iah = NULL;
    unsigned int bodysize;
    unsigned char *ptr;
    int flags = 0;
    krb5_data data;

    if (token == GSS_C_NO_BUFFER || token->length == 0) {
        code = KRB5_BAD_MSIZE;
        goto cleanup;
    }

    if (initialContextToken)
        flags |= G_VFY_TOKEN_HDR_WRAPPER_REQUIRED;

    code = g_verify_token_header(gss_mech_iakerb,
                                 &bodysize, &ptr,
                                 IAKERB_TOK_PROXY,
                                 token->length, flags);
    if (code != 0)
        goto cleanup;

    data.data = (char *)ptr;
    data.length = der_read_length(&ptr, &bodysize);
    if (data.length < 0) {
        code = KRB5_BAD_MSIZE;
        goto cleanup;
    }

    code = decode_krb5_iakerb_header(&data, &iah);
    if (code != 0)
        goto cleanup;

    if (realm != NULL) {
        *realm = iah->target_realm;
        iah->target_realm.data = NULL;
    }

    if (cookie != NULL) {
        *cookie = iah->cookie;
        iah->cookie = NULL;
    }

    request->data = (char *)ptr;
    request->length = bodysize;

cleanup:
    krb5_free_iakerb_header(ctx->k5c, iah);

    return code;
}

static krb5_error_code
iakerb_make_token(iakerb_ctx_id_t ctx,
                  krb5_data *realm,
                  krb5_data *cookie,
                  krb5_data *request,
                  int initialContextToken,
                  gss_buffer_t token)
{
    krb5_error_code code;
    krb5_iakerb_header iah;
    krb5_data *data = NULL;
    char *p;

    token->value = NULL;
    token->length = 0;

    /*
     * Assemble the IAKERB-HEADER from the realm and cookie
     */
    memset(&iah, 0, sizeof(iah));
    iah.target_realm = *realm;
    iah.cookie = cookie;

    code = encode_krb5_iakerb_header(&iah, &data);
    if (code != 0)
        goto cleanup;

    /*
     * Add the TOK_ID to the beginning of the header and the
     * Kerberos request to the end.
     */
    p = realloc(data->data, 2 + data->length + request->length);
    if (p == NULL) {
        code = ENOMEM;
        goto cleanup;
    }

    memmove(p + 2, data->data, data->length);
    memcpy(p + 2 + data->length, request->data, request->length);
    store_16_be(IAKERB_TOK_PROXY, p);

    data->length += 2 /* TOK_ID */ + request->length;
    data->data = p;

    if (initialContextToken) {
        unsigned int tokenSize;
        unsigned char *q;

        tokenSize = g_token_size(gss_mech_iakerb, data->length);
        token->value = k5alloc(tokenSize, &code);
        if (code != 0)
            goto cleanup;
        q = token->value;
        g_make_token_header(gss_mech_iakerb, data->length, &q, -1);
        memcpy(q, data->data, data->length);
        token->length = tokenSize + data->length;
    } else {
        token->value = data->data;
        token->length = data->length;
        data->data = NULL; /* do not double-free */
    }

cleanup:
    krb5_free_data(ctx->k5c, data);

    return code;
}

static krb5_error_code
iakerb_acceptor_step(iakerb_ctx_id_t ctx,
                     int initialContextToken,
                     const gss_buffer_t input_token,
                     gss_buffer_t output_token)
{
    krb5_error_code code;
    krb5_data request, reply, realm;
    OM_uint32 tmp;
    int tcpOnly = 0, useMaster = 0;

    output_token->length = 0;
    output_token->value = NULL;

    request.data = NULL;
    request.length = 0;
    reply.data = NULL;
    reply.length = 0;
    realm.data = NULL;
    realm.length = 0;

    code = iakerb_parse_token(ctx,
                              initialContextToken,
                              input_token,
                              &realm,
                              NULL,
                              &request);
    if (code != 0)
        goto cleanup;

    if (realm.length == 0 || request.length == 0) {
        code = KRB5_BAD_MSIZE;
        goto cleanup;
    }

    code = iakerb_save_token(ctx, input_token);
    if (code != 0)
        goto cleanup;

send_again:
    code = krb5_sendto_kdc(ctx->k5c, &request, &realm,
                           &reply, &useMaster, tcpOnly);
    if (code == KRB5_KDC_UNREACH || code == KRB5_REALM_UNKNOWN) {
        krb5_error error;

        memset(&error, 0, sizeof(error));
        if (code == KRB5_KDC_UNREACH)
            error.error = KRB_AP_ERR_IAKERB_KDC_NO_RESPONSE;
        else if (code == KRB5_REALM_UNKNOWN)
            error.error = KRB_AP_ERR_IAKERB_KDC_NOT_FOUND;

        code = krb5_mk_error(ctx->k5c, &error, &reply);
        if (code != 0)
            goto cleanup;
    } else if (code == 0 && krb5_is_krb_error(&reply)) {
        krb5_error *error;

        code = decode_krb5_error(&reply, &error);
        if (code != 0)
            goto cleanup;

        if (error && error->error == KRB_ERR_RESPONSE_TOO_BIG &&
            tcpOnly == 0) {
            tcpOnly = 1;
            krb5_free_error(ctx->k5c, error);
            krb5_free_data_contents(ctx->k5c, &reply);
            goto send_again;
        }
    } else if (code != 0)
        goto cleanup;

    if (krb5_is_as_rep(&reply))
        ctx->complete = 1;

    code = iakerb_make_token(ctx, &realm, NULL, &reply,
                             0, output_token);
    if (code != 0)
        goto cleanup;

    code = iakerb_save_token(ctx, output_token);
    if (code != 0)
        goto cleanup;

cleanup:
    if (code != 0)
        gss_release_buffer(&tmp, output_token);
    /* request is a pointer into input_token, no need to free */
    krb5_free_data_contents(ctx->k5c, &realm);
    krb5_free_data_contents(ctx->k5c, &reply);

    return code;
}

static krb5_error_code
iakerb_initiator_step(iakerb_ctx_id_t ctx,
                      krb5_gss_cred_id_t cred,
                      const gss_buffer_t input_token,
                      gss_buffer_t output_token)
{
    krb5_error_code code;
    krb5_data in, out, realm, *cookie = NULL;
    OM_uint32 tmp;
    int initialContextToken = (input_token == GSS_C_NO_BUFFER);

    output_token->length = 0;
    output_token->value = NULL;

    in.data = NULL;
    in.length = 0;
    out.data = NULL;
    out.length = 0;
    realm.data = NULL;
    realm.length = 0;

    if (initialContextToken) {
        in.data = NULL;
        in.length = 0;
    } else {
        code = iakerb_parse_token(ctx,
                                  0,
                                  input_token,
                                  NULL,
                                  &cookie,
                                  &in);
        if (code != 0)
            goto cleanup;

        code = iakerb_save_token(ctx, input_token);
        if (code != 0)
            goto cleanup;
    }

    code = krb5_init_creds_step(ctx->k5c,
                                ctx->icc,
                                &in,
                                &out,
                                &realm,
                                &ctx->complete);
    if (code != 0)
        goto cleanup;

    if (ctx->complete) {
        /* finished */
        krb5_creds creds;

        memset(&creds, 0, sizeof(creds));

        assert(cred->iakerb);

        code = krb5_init_creds_get_creds(ctx->k5c, ctx->icc, &creds);
        if (code != 0)
            goto cleanup;

        code = krb5_cc_store_cred(ctx->k5c, cred->ccache, &creds);
        if (code != 0) {
            krb5_free_cred_contents(ctx->k5c, &creds);
            goto cleanup;
        }
        krb5_free_cred_contents(ctx->k5c, &creds);
    } else {
        code = iakerb_make_token(ctx, &realm, cookie, &out,
                                 (input_token == GSS_C_NO_BUFFER),
                                 output_token);
        if (code != 0)
            goto cleanup;
    }

    /* Save the token for generating a future checksum */
    code = iakerb_save_token(ctx, output_token);
    if (code != 0)
        goto cleanup;

cleanup:
    if (code != 0)
        gss_release_buffer(&tmp, output_token);
    krb5_free_data(ctx->k5c, cookie);
    krb5_free_data_contents(ctx->k5c, &out);
    krb5_free_data_contents(ctx->k5c, &realm);

    return code;
}

static krb5_error_code
iakerb_init_creds_ctx(iakerb_ctx_id_t ctx,
                      krb5_gss_cred_id_t cred,
                      krb5_gss_name_t target)
{
    krb5_error_code code;
    krb5_get_init_creds_opt *opts = NULL;

    if (cred->iakerb == 0 || cred->password.data == NULL) {
        code = EINVAL;
        goto cleanup;
    }

    code = krb5_init_creds_init(ctx->k5c,
                                cred->name->princ,
                                NULL,
                                NULL,
                                0,
                                opts,
                                &ctx->icc);
    if (code != 0)
        goto cleanup;

    code = krb5_init_creds_set_password(ctx->k5c,
                                        ctx->icc,
                                        cred->password.data);
    if (code != 0)
        goto cleanup;

cleanup:
    krb5_get_init_creds_opt_free(ctx->k5c, opts);

    return code;
}

static krb5_error_code
iakerb_alloc_context(iakerb_ctx_id_t *pctx)
{
    iakerb_ctx_id_t ctx;
    krb5_error_code code;

    *pctx = NULL;

    ctx = k5alloc(sizeof(*ctx), &code);
    if (ctx == NULL)
        goto cleanup;
    ctx->magic = KV5M_GSS_IAKERB_CONTEXT;

    code = krb5_gss_init_context(&ctx->k5c);
    if (code != 0)
        goto cleanup;

    ctx->gssc = GSS_C_NO_CONTEXT;

    *pctx = ctx;

cleanup:
    if (code != 0)
        iakerb_release_context(ctx);

    return code;
}

OM_uint32
iakerb_delete_sec_context(OM_uint32 *minor_status,
                          gss_ctx_id_t *context_handle,
                          gss_buffer_t output_token)
{
    OM_uint32 major_status = GSS_S_COMPLETE;

    output_token->length = 0;
    output_token->value = NULL;
    *minor_status = 0;

    if (*context_handle != GSS_C_NO_CONTEXT) {
        iakerb_ctx_id_t iakerb_ctx = (iakerb_ctx_id_t)*context_handle;

        if (iakerb_ctx->magic == KV5M_GSS_IAKERB_CONTEXT) {
            iakerb_release_context(iakerb_ctx);
            *context_handle = GSS_C_NO_CONTEXT;
        } else {
            major_status = krb5_gss_delete_sec_context(minor_status,
                                                       context_handle,
                                                       output_token);
        }
    }

    return major_status;
}

OM_uint32
iakerb_accept_sec_context(OM_uint32 *minor_status,
                          gss_ctx_id_t *context_handle,
                          gss_cred_id_t verifier_cred_handle,
                          gss_buffer_t input_token,
                          gss_channel_bindings_t input_chan_bindings,
                          gss_name_t *src_name,
                          gss_OID *mech_type,
                          gss_buffer_t output_token,
                          OM_uint32 *ret_flags,
                          OM_uint32 *time_rec,
                          gss_cred_id_t *delegated_cred_handle)
{
    OM_uint32 major_status = GSS_S_FAILURE;
    OM_uint32 code;
    iakerb_ctx_id_t ctx;
    int initialContextToken = (*context_handle == GSS_C_NO_CONTEXT);

    if (initialContextToken) {
        code = iakerb_alloc_context(&ctx);
        if (code != 0)
            goto cleanup;
    } else
        ctx = (iakerb_ctx_id_t)*context_handle;

    if (ctx->complete) {
        krb5_gss_ctx_ext_rec exts;

        memset(&exts, 0, sizeof(exts));
        exts.iakerb_conv = &ctx->conv;

        major_status = krb5_gss_accept_sec_context_ext(&code,
                                                       &ctx->gssc,
                                                       verifier_cred_handle,
                                                       input_token,
                                                       input_chan_bindings,
                                                       src_name,
                                                       mech_type,
                                                       output_token,
                                                       ret_flags,
                                                       time_rec,
                                                       delegated_cred_handle,
                                                       &exts);
        if (major_status == GSS_S_COMPLETE) {
            *context_handle = ctx->gssc;
            ctx->gssc = NULL;
            iakerb_release_context(ctx);
        }
    } else {
        code = iakerb_acceptor_step(ctx, initialContextToken,
                                    input_token, output_token);
        if (code == (OM_uint32)KRB5_BAD_MSIZE)
            major_status = GSS_S_DEFECTIVE_TOKEN;
        if (code != 0)
            goto cleanup;
        if (src_name != NULL)
            *src_name = GSS_C_NO_NAME;
        if (mech_type != NULL)
            *mech_type = (gss_OID)gss_mech_iakerb;
        if (ret_flags != NULL)
            *ret_flags = 0;
        if (time_rec != NULL)
            *time_rec = 0;
        if (delegated_cred_handle != NULL)
            *delegated_cred_handle = GSS_C_NO_CREDENTIAL;
    }

cleanup:
    if (initialContextToken && ctx != NULL)
        iakerb_release_context(ctx);

    *minor_status = code;
    return major_status;
}

OM_uint32
iakerb_init_sec_context(OM_uint32 *minor_status,
                        gss_cred_id_t claimant_cred_handle,
                        gss_ctx_id_t *context_handle,
                        gss_name_t target_name,
                        gss_OID mech_type,
                        OM_uint32 req_flags,
                        OM_uint32 time_req,
                        gss_channel_bindings_t input_chan_bindings,
                        gss_buffer_t input_token,
                        gss_OID *actual_mech_type,
                        gss_buffer_t output_token,
                        OM_uint32 *ret_flags,
                        OM_uint32 *time_rec)
{
    OM_uint32 major_status = GSS_S_FAILURE;
    OM_uint32 code;
    iakerb_ctx_id_t ctx;
    krb5_gss_cred_id_t kcred;
    int credLocked = 0;
    int initialContextToken = (*context_handle == GSS_C_NO_CONTEXT);

    if (initialContextToken) {
        code = iakerb_alloc_context(&ctx);
        if (code != 0)
            goto cleanup;
    } else
        ctx = (iakerb_ctx_id_t)*context_handle;

    if (!kg_validate_name(target_name)) {
        code = G_VALIDATE_FAILED;
        major_status = GSS_S_CALL_BAD_STRUCTURE | GSS_S_BAD_NAME;
        goto cleanup;
    }

    major_status = krb5_gss_validate_cred_1(&code,
                                            claimant_cred_handle,
                                            ctx->k5c);
    if (GSS_ERROR(major_status))
        goto cleanup;

    credLocked = 1;

    kcred = (krb5_gss_cred_id_t)claimant_cred_handle;
    if (kcred->iakerb == 0) {
        major_status = GSS_S_DEFECTIVE_CREDENTIAL;
        goto cleanup;
    }

    if (initialContextToken) {
        code = iakerb_init_creds_ctx(ctx, kcred, (krb5_gss_name_t)target_name);
        if (code != 0)
            goto cleanup;
    }

    if (ctx->complete == 0) {
        code = iakerb_initiator_step(ctx,
                                     kcred,
                                     input_token,
                                     output_token);
        if (code == (OM_uint32)KRB5_BAD_MSIZE)
            major_status = GSS_S_DEFECTIVE_TOKEN;
        if (code != 0)
            goto cleanup;
    }

    if (ctx->complete) {
        krb5_gss_ctx_ext_rec exts;

        memset(&exts, 0, sizeof(exts));
        exts.iakerb_conv = &ctx->conv;

        k5_mutex_unlock(&kcred->lock);
        credLocked = 0;

        if (ctx->gssc == GSS_C_NO_CONTEXT)
            input_token = GSS_C_NO_BUFFER;

        major_status = krb5_gss_init_sec_context_ext(&code,
                                                     claimant_cred_handle,
                                                     &ctx->gssc,
                                                     target_name,
                                                     (gss_OID)gss_mech_krb5,
                                                     req_flags,
                                                     time_req,
                                                     input_chan_bindings,
                                                     input_token,
                                                     actual_mech_type,
                                                     output_token,
                                                     ret_flags,
                                                     time_rec,
                                                     &exts);
        if (major_status == GSS_S_COMPLETE) {
            *context_handle = ctx->gssc;
            ctx->gssc = NULL;
            iakerb_release_context(ctx);
        }
    } else {
        if (initialContextToken)
            *context_handle = (gss_ctx_id_t)ctx;
        if (actual_mech_type != NULL)
            *actual_mech_type = (gss_OID)gss_mech_iakerb;
        if (ret_flags != NULL)
            *ret_flags = 0;
        if (time_rec != NULL)
            *time_rec = 0;
        major_status = GSS_S_CONTINUE_NEEDED;
    }

cleanup:
    if (credLocked)
        k5_mutex_unlock(&kcred->lock);
    if (initialContextToken && ctx != NULL)
        iakerb_release_context(ctx);

    *minor_status = code;
    return major_status;
}
