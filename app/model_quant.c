#include "model_quant.h"
/* Minimal TFLite flatbuffer reader: output-tensor quantization scale + zero-point.
 * Enough of the FlatBuffer format to walk Model -> subgraphs[0] -> (tensors, outputs)
 * -> Tensor.quantization -> scale[0], zero_point[0]. No schema/codegen dependency. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- flatbuffer primitives --- */
static uint32_t rd_u32(const uint8_t* p){ uint32_t v; memcpy(&v,p,4); return v; }
static int32_t  rd_i32(const uint8_t* p){ int32_t  v; memcpy(&v,p,4); return v; }
static uint16_t rd_u16(const uint8_t* p){ uint16_t v; memcpy(&v,p,2); return v; }
static float    rd_f32(const uint8_t* p){ float    v; memcpy(&v,p,4); return v; }
static int64_t  rd_i64(const uint8_t* p){ int64_t  v; memcpy(&v,p,8); return v; }

/* table at 'tbl'; return absolute offset of field 'fid' (0-based), or 0 if absent */
static const uint8_t* tbl_field(const uint8_t* base, const uint8_t* tbl, int fid){
    int32_t vt_rel = rd_i32(tbl);           /* soffset: table - vtable */
    const uint8_t* vt = tbl - vt_rel;
    uint16_t vt_size = rd_u16(vt);
    int voff = 4 + fid*2;                     /* vtable: [vt_size][tbl_size][fields...] */
    if (voff + 2 > vt_size) return NULL;
    uint16_t foff = rd_u16(vt + voff);
    if (!foff) return NULL;
    return tbl + foff;
}
/* follow a uoffset (relative table/vector pointer) */
static const uint8_t* follow(const uint8_t* p){ return p + rd_u32(p); }

int tflite_output_quant(const char* path,
                        float* coord_scale, int* coord_zero,
                        float* score_scale, int* score_zero,
                        int coord_channels /*4*/, int score_channels /*nc*/) {
    FILE* f=fopen(path,"rb"); if(!f) return 0;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t* buf=malloc(n); if(fread(buf,1,n,f)!=(size_t)n){fclose(f);free(buf);return 0;} fclose(f);
    const uint8_t* base=buf;

    const uint8_t* model = follow(base);                 /* root table (Model) */
    /* Model field 1 = subgraphs (vector of SubGraph) */
    const uint8_t* sgf = tbl_field(base, model, 2);   /* Model.subgraphs = field 2 */
    if(!sgf){free(buf);return 0;}
    const uint8_t* sgvec = follow(sgf);
    uint32_t sg_count = rd_u32(sgvec);
    if(sg_count<1){free(buf);return 0;}
    const uint8_t* sg = follow(sgvec + 4);               /* subgraphs[0] */

    /* SubGraph field 0 = tensors, field 3 = outputs (vector<int>) */
    const uint8_t* tf = tbl_field(base, sg, 0);
    const uint8_t* of = tbl_field(base, sg, 2);   /* SubGraph.outputs = field 2 */
    if(!tf||!of){free(buf);return 0;}
    const uint8_t* tvec = follow(tf); uint32_t t_count=rd_u32(tvec);
    const uint8_t* ovec = follow(of); uint32_t o_count=rd_u32(ovec);

    int got=0;
    for(uint32_t k=0;k<o_count;k++){
        int32_t tidx = rd_i32(ovec + 4 + k*4);
        if(tidx<0||(uint32_t)tidx>=t_count) continue;
        const uint8_t* tensor = follow(tvec + 4 + tidx*4);
        /* Tensor field 0 = shape (vector<int>), field 4 = quantization */
        const uint8_t* shf = tbl_field(base, tensor, 0);
        const uint8_t* qf  = tbl_field(base, tensor, 4);
        if(!shf||!qf) continue;
        const uint8_t* shp = follow(shf); uint32_t sdim=rd_u32(shp);
        /* channel count is shape[1] for [1, C, N] */
        int chan = sdim>=2 ? rd_i32(shp+4+1*4) : -1;
        const uint8_t* q = follow(qf);
        /* QuantizationParameters field 1 = scale (vector<float>), field 2 = zero_point (vector<long>) */
        const uint8_t* scf = tbl_field(base, q, 2);   /* Quantization.scale = field 2 */
        const uint8_t* zpf = tbl_field(base, q, 3);   /* Quantization.zero_point = field 3 */
        if(!scf) continue;
        const uint8_t* scv = follow(scf);
        float scale = rd_f32(scv+4);
        int zero = 0;
        if(zpf){ const uint8_t* zpv=follow(zpf); zero=(int)rd_i64(zpv+4); }
        if(chan==coord_channels){ *coord_scale=scale; *coord_zero=zero; got|=1; }
        else if(chan==score_channels){ *score_scale=scale; *score_zero=zero; got|=2; }
    }
    free(buf);
    return got==3;
}
