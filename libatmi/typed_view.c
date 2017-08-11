/* 
** VIEW buffer type support
** Basically all data is going to be stored in FB.
** For view not using base as 3000. And the UBF field numbers follows the 
** the field number in the view (1, 2, 3, 4, ...) 
** Sample file:
** 
**# 
**#type    cname   fbname count flag  size  null
**#
**int     in    -   1   -   -   -
**short    sh    -   2   -   -   -
**long     lo    -   3   -   -   -
**char     ch    -   1   -   -   -
**float    fl    -   1   -   -   -
**double    db    -   1   -   -   -
**string    st    -   1   -   15   -
**carray    ca    -   1   -   15   -
**END
** 
**
** @file typed_view.c
** 
** -----------------------------------------------------------------------------
** Enduro/X Middleware Platform for Distributed Transaction Processing
** Copyright (C) 2015, Mavimax, Ltd. All Rights Reserved.
** This software is released under one of the following licenses:
** GPL or Mavimax's license for commercial use.
** -----------------------------------------------------------------------------
** GPL license:
** 
** This program is free software; you can redistribute it and/or modify it under
** the terms of the GNU General Public License as published by the Free Software
** Foundation; either version 2 of the License, or (at your option) any later
** version.
**
** This program is distributed in the hope that it will be useful, but WITHOUT ANY
** WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
** PARTICULAR PURPOSE. See the GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License along with
** this program; if not, write to the Free Software Foundation, Inc., 59 Temple
** Place, Suite 330, Boston, MA 02111-1307 USA
**
** -----------------------------------------------------------------------------
** A commercial use license is available from Mavimax, Ltd
** contact@mavimax.com
** -----------------------------------------------------------------------------
*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>
#include <dirent.h>

#include <ndrstandard.h>
#include <typed_buf.h>
#include <ndebug.h>
#include <tperror.h>
#include <ubfutil.h>

#include <userlog.h>
#include <typed_view.h>
#include <view_cmn.h>
#include <atmi_tls.h>

#include "Exfields.h"
/*---------------------------Externs------------------------------------*/
/*---------------------------Macros-------------------------------------*/
/*---------------------------Enums--------------------------------------*/
/*---------------------------Typedefs-----------------------------------*/
/*---------------------------Globals------------------------------------*/
/*---------------------------Statics------------------------------------*/
/*---------------------------Prototypes---------------------------------*/
/**
 * Allocate the view object
 * @param descr
 * @param subtype
 * @param len
 * @return 
 */
expublic char * VIEW_tpalloc (typed_buffer_descr_t *descr, char *subtype, long len)
{
    char *ret=NULL;
    
    ndrx_typedview_t *v = ndrx_view_get_view(subtype);
    
    if (NULL==v)
    {
         NDRX_LOG(log_error, "%s: VIEW [%s] NOT FOUND!", __func__, subtype);
        ndrx_TPset_error_fmt(TPENOENT, "%s: VIEW [%s] NOT FOUND!", 
                __func__, subtype);
        goto out;
    }
    
    if (NDRX_VIEW_SIZE_DEFAULT_SIZE>len)
    {
        len = NDRX_VIEW_SIZE_DEFAULT_SIZE;
    }
    
    /* Allocate VIEW buffer */
    /* Maybe still malloc? */
    ret=(char *)NDRX_CALLOC(1, len);

    if (NULL==ret)
    {
        NDRX_LOG(log_error, "%s: Failed to allocate VIEW buffer!", __func__);
        ndrx_TPset_error_msg(TPEOS, Bstrerror(Berror));
        goto out;
    }
    
    /* Check the size of the view, if buffer is smaller then view
     * the have some working in ULOG and logfile... */
    if (v->ssize < len)
    {
        NDRX_LOG(log_info, "tpalloc'ed %ld bytes, but VIEW [%s] structure size if %ld",
                len, subtype, v->ssize);
    }

out:
    return ret;
}

/**
 * Re-allocate CARRAY buffer. Firstly we will find it in the list.
 * @param ptr
 * @param size
 * @return
 */
expublic char * VIEW_tprealloc(typed_buffer_descr_t *descr, char *cur_ptr, long len)
{
    char *ret=NULL;

    if (0==len)
    {
        len = NDRX_VIEW_SIZE_DEFAULT_SIZE;
    }

    /* Allocate CARRAY buffer */
    ret=(char *)NDRX_REALLOC(cur_ptr, len);
    

    return ret;
}

/**
 * this will add data the the buffer. If data size is too small, allocate extra
 * 1024 bytes and add again.
 * @param pp_ub double ptr to buffer
 * @param bfldid field id
 * @param occ occurrence
 * @param buf buffer
 * @param len len
 * @return EXSUCCED/EXFAIL
 */
exprivate  int sized_Bchg (UBFH **pp_ub, BFLDID bfldid, 
        BFLDOCC occ, char * buf, BFLDLEN len)
{
    int ret = EXSUCCEED;
    

    while (EXSUCCEED!=(ret=Bchg(*pp_ub, bfldid, occ, buf, len))
            &&  BNOSPACE==Berror)
    {
        if (NULL==(*pp_ub = (UBFH *)tprealloc((char *)*pp_ub, Bsizeof(*pp_ub) + 1024)))
        {
            NDRX_LOG(log_error, "Failed to realloc the buffer!");            
            EXFAIL_OUT(ret);
        }
    }
    
out:
    return ret;
}

/**
 * Build the outgoing buffer... This will convert the VIEW to internal UBF
 * format.
 * 
 * @param descr
 * @param idata
 * @param ilen
 * @param obuf
 * @param olen
 * @param flags
 * @return 
 */
expublic int VIEW_prepare_outgoing (typed_buffer_descr_t *descr, char *idata, long ilen, 
                    char *obuf, long *olen, long flags)
{
    int ret = EXSUCCEED;
    UBFH *p_ub = NULL;
    buffer_obj_t *bo;
    ndrx_typedview_t *v;
    ndrx_typedview_field_t *f;
    long cksum;
    BFLDID fldid;
    int i;
    typed_buffer_descr_t *ubf_descr;
    
    /* Indicators.. */
    short *C_count;
    short C_count_stor;
    unsigned short *L_length; /* will transfer as long */
    long L_len_long;
    
    int *int_fix_ptr;
    long int_fix_l;
    
    int occ;
    
    /* get the view */
    
    bo = ndrx_find_buffer(idata);
 
    if (NULL==bo)
    {
        ndrx_TPset_error_fmt(TPEINVAL, "Input buffer not allocated by tpalloc!");
        EXFAIL_OUT(ret);
    }
    
    NDRX_DUMP(log_dump, "Outgoing VIEW struct", idata, ilen);
    
    NDRX_LOG(log_debug, "Preparing outgoing for VIEW [%s]", bo->sub_type);
    
    v = ndrx_view_get_view(bo->sub_type);
    
    if (NULL==v)
    {
        ndrx_TPset_error_fmt(TPEINVAL, "View not found [%s]!", bo->sub_type);
        EXFAIL_OUT(ret);
    }
   
    /* Build outgoing UBF...
     * We will make chunks by 1024 bytes. Thus if Bchg op fails with nospace
     * error then reallocate and try again. 
     */
   
    p_ub = (UBFH*)tpalloc("UBF", NULL, 1024);
    
    if (NULL==p_ub)
    {
        NDRX_LOG(log_error, "Failed to allocate UBF buffer!");
        EXFAIL_OUT(ret);
    }
    
    if (EXSUCCEED!=Bchg(p_ub, EX_VIEW_NAME, 0, bo->sub_type, 0L))
    {
        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup EX_VIEW_NAME to [%s]: %s", 
                v->vname, Bstrerror(Berror));
        EXFAIL_OUT(ret);
    }
    
    cksum = (long)v->cksum;
    
    if (EXSUCCEED!=Bchg(p_ub, EX_VIEW_CKSUM, 0, (char *)&cksum, 0L))
    {
        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup EX_VIEW_CKSUM to [%ld]: %s", 
                cksum, Bstrerror(Berror));
        EXFAIL_OUT(ret);
    }
    
    /* Now setup the fields in the buffer according to loaded view object 
     * we will load all fields into the buffer
     */
    i = NDRX_VIEW_UBF_BASE;
    DL_FOREACH(v->fields, f)
    {
        i++;
        
        NDRX_LOG(log_dump, "Processing field: [%s]", f->cname);
        /* Check do we have length indicator? */
        if (f->flags & NDRX_VIEW_FLAG_ELEMCNT_IND_C)
        {
            C_count = (short *)(idata+f->count_fld_offset);
            NDRX_LOG(log_dump, "C_count=%hd", *C_count);
            
            fldid = Bmkfldid(BFLD_SHORT, i);
            
            if (EXSUCCEED!=Bchg(p_ub, fldid, 0, (char *)C_count, 0L))
            {
                ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup C_count at field %d: %s", 
                    i, Bstrerror(Berror));
                EXFAIL_OUT(ret);
            }
            i++;
        }
        else
        {
            C_count_stor=f->count; 
            C_count = &C_count_stor;
        }
        
        if (f->flags & NDRX_VIEW_FLAG_LEN_INDICATOR_L)
        {
            for (occ=0; occ<*C_count; occ++)
            {
                L_length = (unsigned short *)(idata+f->length_fld_offset+occ);
                L_len_long = (long)*L_length;
                NDRX_LOG(log_dump, "L_length=%hu (long: %ld), occ=%d", 
                        *L_length, L_len_long, occ);

                fldid = Bmkfldid(BFLD_LONG, i);

                if (EXSUCCEED!=Bchg(p_ub, fldid, occ, (char *)&L_len_long, 0L))
                {
                    ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup L_len_long "
                            "at field %d, occ %d: %s", 
                        i, occ, Bstrerror(Berror));
                    EXFAIL_OUT(ret);
                }
            }
            i++;
        }
        
        fldid = Bmkfldid(f->typecode, i);
        
        NDRX_LOG(log_debug, "C_count=%hd fldid=%d", *C_count, fldid);
        
        /* well we must support arrays too...! of any types
         * Arrays we will load into occurrences of the same buffer
         */
        /* loop over the occurrences 
         * we might want to send less count then max struct size..*/
        for (occ=0; occ<*C_count; occ++)
        {
            int dim_size = f->fldsize/f->count;
            /* OK we need to understand following:
             * size (data len) of each of the array elements..
             * the header plotter needs to support occurrences of the
             * length elements for the arrays...
             */
            char *fld_offs = idata+f->offset+occ*dim_size;
            
            if (BFLD_INT==f->typecode)
            {
                NDRX_LOG(log_dump, "Setting up INT");
                int_fix_ptr = (int *)(fld_offs);
                int_fix_l = (long)*int_fix_ptr;

                if (EXSUCCEED!=sized_Bchg(&p_ub, fldid, occ, (char *)&int_fix_l, 0L))
                {
                    ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup field %d", 
                        fldid);
                    EXFAIL_OUT(ret);
                }
            }
            else if (BFLD_CARRAY!=f->typecode)
            {
                NDRX_LOG(log_dump, "Setting up %hd", f->typecode);
                /* here length indicator is not needed */

                if (EXSUCCEED!=sized_Bchg(&p_ub, fldid, occ, fld_offs, 0L))
                {
                    ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup field %d", 
                        fldid);
                    EXFAIL_OUT(ret);
                }
            }
            else 
            {
                /* This is carray... manage the length  
                 * we need array access to to the length indicators...
                 * if the L flag is set, then we must use length flags
                 * if L not set, then setup full buffer...
                 */
                if (f->flags & NDRX_VIEW_FLAG_LEN_INDICATOR_L)
                {
                    NDRX_LOG(log_dump, "Setting CARRAY with length indicator");
                    
                    L_length = (unsigned short *)(idata+f->length_fld_offset+
                            occ*sizeof(unsigned short));
                    L_len_long = (long)*L_length;
                    
                    if (EXSUCCEED!=sized_Bchg(&p_ub, fldid, occ, fld_offs, L_len_long))
                    {
                        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup "
                                "carray field %d, occ %d, offs %d, L_len_long %ld", 
                            fldid, occ, fld_offs, L_len_long);
                        EXFAIL_OUT(ret);
                    }
                    
                }
                else
                {
                    NDRX_LOG(log_dump, "Setting CARRAY w/o length indicator");
                    
                    if (EXSUCCEED!=sized_Bchg(&p_ub, fldid, occ, fld_offs, dim_size))
                    {
                        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup "
                                "carray field %d, occ %d, offs %d, dim_size %d", 
                            fldid, occ, fld_offs, dim_size);
                        EXFAIL_OUT(ret);
                    }
                }
            }
        } /* for occ */
    }
    
    ndrx_debug_dump_UBF(log_info, "Sending intermediate UBF buffer "
            "containing VIEW data", p_ub);
    
    ubf_descr = ndrx_get_buffer_descr("UBF", NULL);

    /* get the UBF buffer descr... */
    if (EXSUCCEED!=UBF_prepare_outgoing (ubf_descr, (char *)p_ub, 
        0, obuf, olen, 0L))
    {
        NDRX_LOG(log_error, "Failed to build UBF buffer!");
        EXFAIL_OUT(ret);
    }
    
out:
    if (NULL!=p_ub)
    {
        tpfree((char *)p_ub);
    }
    return ret;
}

/**
 * Prepare incoming buffer. the rcv_data is non xatmi buffer. Thus we will
 * just make ptr 
 * @param descr
 * @param rcv_data
 * @param rcv_len
 * @param odata
 * @param olen
 * @param flags
 * @return 
 */
expublic int VIEW_prepare_incoming (typed_buffer_descr_t *descr, char *rcv_data, 
                        long rcv_len, char **odata, long *olen, long flags)
{
    int ret=EXSUCCEED;
    int rcv_buf_size;
    int existing_size;
    UBFH *p_ub = (UBFH *)rcv_data;
    char *p_out;
    buffer_obj_t *outbufobj=NULL;
    char subtype[NDRX_VIEW_NAME_LEN+1];
    ndrx_typedview_t *v;
    ndrx_typedview_field_t *f;
    long cksum;
    int i;
    BFLDID fldid;
    /* Indicators.. */
    short *C_count;
    short C_count_stor;
    unsigned short *L_length; /* will transfer as long */
    long L_len_long;
    
    int *int_fix_ptr;
    long int_fix_l;
    
    int occ;
    
    NDRX_LOG(log_debug, "Entering %s", __func__);
    
    /* test the UBF buffer: */
    if (EXFAIL==(rcv_buf_size=Bused(p_ub)))
    {
        ndrx_TPset_error_msg(TPEINVAL, Bstrerror(Berror));
        ret=EXFAIL;
        goto out;
    }
    
    NDRX_LOG(log_debug, "UBF size: %d", rcv_buf_size);
    
    /* Get the sub-type we received */
    
    ndrx_debug_dump_UBF(log_info, "Received intermediate UBF buffer "
            "containing VIEW data", p_ub);
    
    /* Dump the incoming buffer... */
    if (EXSUCCEED!=Bget(p_ub, EX_VIEW_NAME, 0, subtype, 0L))
    {
        ndrx_TPset_error_fmt(TPEINVAL, "Failed to get view name "
                "from incoming buffer..");
        ret=EXFAIL;
        goto out;
    }
    
    /* Resolve the view data, get the size... */
    
    v = ndrx_view_get_view(subtype);
    
    if (NULL==v)
    {
        userlog("View [%s] not defined!", subtype);
        ndrx_TPset_error_fmt(TPEINVAL, "View [%s] not defined!", subtype);
        EXFAIL_OUT(ret);
    }
    
    NDRX_LOG(log_debug, "Received VIEW [%s]", subtype);

    /* Figure out the passed in buffer */
    if (NULL!=*odata && NULL==(outbufobj=ndrx_find_buffer(*odata)))
    {
        ndrx_TPset_error_fmt(TPEINVAL, "Output buffer %p is not allocated "
                                        "with tpalloc()!", odata);
        ret=EXFAIL;
        goto out;
    }

    /* Check the data types */
    if (NULL!=outbufobj)
    {
        /* If we cannot change the data type, then we trigger an error */
        if (flags & TPNOCHANGE && (outbufobj->type_id!=BUF_TYPE_VIEW ||
                0!=strcmp(outbufobj->sub_type, subtype)))
        {
            /* Raise error! */
            ndrx_TPset_error_fmt(TPEINVAL, "Receiver expects %s/%s but got %s/%s buffer",
                    G_buf_descr[BUF_TYPE_VIEW].type, 
                    subtype,
                    G_buf_descr[outbufobj->type_id].type,
                    outbufobj->sub_type);
            EXFAIL_OUT(ret);
        }
        
        /* If we can change data type and this does not match, then
         * we should firstly free it up and then bellow allow to work in mode
         * when odata is NULL!
         */
        if (outbufobj->type_id!=BUF_TYPE_VIEW || 0!=strcmp(outbufobj->sub_type, subtype) )
        {
            NDRX_LOG(log_warn, "User buffer %s/%s is different, "
                    "free it up and re-allocate as VIEW/%s", 
                    G_buf_descr[outbufobj->type_id].type,
                    (outbufobj->sub_type==NULL?"NULL":outbufobj->sub_type),
                    subtype);
            
            ndrx_tpfree(*odata, outbufobj);
            *odata=NULL;
        }
    }
    
    /* check the output buffer */
    if (NULL!=*odata)
    {
        p_out = (char *)*odata;
        NDRX_LOG(log_debug, "%s: Output buffer exists", __func__);
        
        existing_size=outbufobj->size;

        NDRX_LOG(log_debug, "%s: Output buffer size (struct size): %d, "
                "received %d", __func__,
                            existing_size, rcv_buf_size);
        
        if (existing_size>=rcv_buf_size)
        {
            /* re-use existing buffer */
            NDRX_LOG(log_debug, "%s: Using existing buffer", __func__);
        }
        else
        {
            /* Reallocate the buffer, because we have missing some space */
            char *new_addr;
            NDRX_LOG(log_debug, "%s: Reallocating", __func__);
            
            if (NULL==(new_addr=ndrx_tprealloc(*odata, rcv_buf_size)))
            {
                NDRX_LOG(log_error, "%s: _tprealloc failed!", __func__);
                EXFAIL_OUT(ret);
            }

            /* allocated OK, return new address */
            *odata = new_addr;
            p_out = *odata;
        }
    }
    else
    {
        /* allocate the buffer */
        NDRX_LOG(log_debug, "%s: Incoming buffer where missing - "
                                         "allocating new!", __func__);

        *odata = ndrx_tpalloc(&G_buf_descr[BUF_TYPE_VIEW], NULL, 
                subtype, rcv_buf_size);

        if (NULL==*odata)
        {
            /* error should be set already */
            NDRX_LOG(log_error, "Failed to allocate new buffer!");
            goto out;
        }

        p_out = *odata;
    }
    
    /* Decode now buffer to structure... 
     * we will go over our structure data
     * then read the corresponding buffers
     * but firstly lets validate the view checksum
     */
    if (EXSUCCEED!=Bget(p_ub, EX_VIEW_CKSUM, 0, (char *)&cksum, 0L))
    {
        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to get EX_VIEW_CKSUM to [%ld]: %s", 
                cksum, Bstrerror(Berror));
        EXFAIL_OUT(ret);
    }
    
    /* Verify checksum */
    if (v->cksum!=cksum)
    {
        NDRX_LOG(log_error, "Invalid checksum for VIEW [%s] received. Our cksum: "
                "%ld, their: %ld - try to recompile VIEW with viewc!",
                v->vname, (long)v->cksum, (long)cksum);
        
        ndrx_TPset_error_fmt(TPEINVAL, "Invalid checksum for VIEW [%s] "
                "received. Our cksum: "
                "%ld, their: %ld - try to recompile VIEW with viewc!",
                v->vname, (long)v->cksum, (long)cksum);
        
    }
    
    /* Now setup the fields in the buffer according to loaded view object 
     * we will load all fields into the buffer
     */
    i = 0;
    DL_FOREACH(v->fields, f)
    {
        i++;
        
        NDRX_LOG(log_dump, "Processing field: [%s]", f->cname);
        /* Check do we have length indicator? */
        if (f->flags & NDRX_VIEW_FLAG_ELEMCNT_IND_C)
        {
            C_count = (short *)(p_out+f->count_fld_offset);
            NDRX_LOG(log_dump, "C_count=%hd", *C_count);
            
            fldid = Bmkfldid(BFLD_SHORT, i);
            
            if (EXSUCCEED!=Bget(p_ub, fldid, 0, (char *)C_count, 0L))
            {
                ndrx_TPset_error_fmt(TPESYSTEM, "Failed to get C_count at field %d: %s", 
                    i, Bstrerror(Berror));
                EXFAIL_OUT(ret);
            }
            i++;
        }
        else
        {
            
            C_count_stor=f->count;
            C_count = &C_count_stor;
        }
        
        if (f->flags & NDRX_VIEW_FLAG_LEN_INDICATOR_L)
        {
            for (occ=0; occ<*C_count; occ++)
            {
                L_length = (unsigned short *)(p_out+f->length_fld_offset+occ);
                L_len_long = (long)*L_length;
                NDRX_LOG(log_dump, "L_length=%hu (long: %ld), occ=%d", 
                        *L_length, L_len_long, occ);

                fldid = Bmkfldid(BFLD_LONG, i);

                if (EXSUCCEED!=Bchg(p_ub, fldid, occ, (char *)&L_len_long, 0L))
                {
                    ndrx_TPset_error_fmt(TPESYSTEM, "Failed to setup L_len_long "
                            "at field %d, occ %d: %s", 
                        i, occ, Bstrerror(Berror));
                    EXFAIL_OUT(ret);
                }
            }
            i++;
        }
        
        fldid = Bmkfldid(f->typecode, i);
        
        /* well we must support arrays too...! of any types
         * Arrays we will load into occurrences of the same buffer
         */
        /* loop over the occurrences 
         * we might want to send less count then max struct size..*/
        NDRX_LOG(log_debug, "C_count=%hd fldid=%d", *C_count, fldid);
        
        for (occ=0; occ<*C_count; occ++)
        {
            int dim_size = f->fldsize/f->count;
            /* OK we need to understand following:
             * size (data len) of each of the array elements..
             * the header plotter needs to support occurrences of the
             * length elements for the arrays...
             */
            char *fld_offs = p_out+f->offset+occ*dim_size;
            
            if (BFLD_INT==f->typecode)
            {
                NDRX_LOG(log_dump, "Getting INT");
                
                if (EXSUCCEED!=Bget(p_ub, fldid, occ, (char *)&int_fix_l, 0L))
                {
                    ndrx_TPset_error_fmt(TPESYSTEM, "Failed to get field %d", 
                        fldid);
                    EXFAIL_OUT(ret);
                }
                else
                {
                    int_fix_ptr = (int *)(fld_offs);
                    *int_fix_ptr = int_fix_l;
                    
                    NDRX_LOG(log_dump, "Got int %d", *int_fix_ptr);
                    
                }
            }
            else 
            {
                /* This is carray... manage the length  
                 * we need array access to to the length indicators...
                 * if the L flag is set, then we must use length flags
                 * if L not set, then setup full buffer...
                 */
                if (f->flags & NDRX_VIEW_FLAG_LEN_INDICATOR_L)
                {
                    BFLDLEN blen = dim_size;
                    NDRX_LOG(log_dump, "Setting with length indicator");
                    
                    L_length = (unsigned short *)(p_out+f->length_fld_offset+
                            occ*sizeof(unsigned short));
                    L_len_long = (long)*L_length;
                    
                    if (EXSUCCEED!=Bget(p_ub, fldid, occ, fld_offs, &blen))
                    {
                        NDRX_LOG(log_error, "Failed to get "
                                "field %d, occ %d, offs %d, blen %ld", 
                            fldid, occ, fld_offs, blen);
                        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to get "
                                "field %d, occ %d, offs %d, blen %ld", 
                            fldid, occ, fld_offs, blen);
                        EXFAIL_OUT(ret);
                    }
                    
                    /* Test the buffer size.. */
                    if (L_len_long!=blen)
                    {
                        NDRX_LOG(log_error, "Length indicator in buffer: %ld, real: %d",
                                L_len_long, blen);
                        ndrx_TPset_error_fmt(TPESYSTEM, "Length indicator "
                                "in buffer: %ld, real: %d",
                                L_len_long, blen);
                        EXFAIL_OUT(ret);
                    }
                    
                }
                else
                {
                    BFLDLEN blen = dim_size;
                    NDRX_LOG(log_dump, "Getting w/o length indicator");
                    
                    if (EXSUCCEED!=Bget(p_ub, fldid, occ, fld_offs, &blen))
                    {
                        ndrx_TPset_error_fmt(TPESYSTEM, "Failed to get "
                                "field %d, occ %d, offs %d, dim_size %d", 
                            fldid, occ, fld_offs, dim_size);
                        EXFAIL_OUT(ret);
                    }
                }
            }
        } /* for occ */
    }
    
    NDRX_DUMP(log_dump, "Incoming VIEW struct", *odata, *olen);
    
out:
    return ret;
}
