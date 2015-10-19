#include "../util/memory.hh"
#include "bool_encoder.hh"
#include "boolwriter.hh"
#include "jpeg_meta.hh"
#include "block.hh"
#include "numeric.hh"
#include "model.hh"
#include "encoder.hh"
#include <map>
#include "weight.hh"
#include <fstream>
#include "../../lepton/idct.hh"
#include "../util/debug.hh"
using namespace std;

uint8_t prefix_remap(uint8_t v) {
    if (v == 0) {
        return 0;
    }
    return v + 3;
}
#ifdef TRACK_HISTOGRAM
map<int, int> histogram[3];// 0 is center, 1 is dc, 2 is edge
struct Blah {
    ~Blah() {
        for (int typ = 0; typ < 3; ++typ) {
            for (map<int,int>::iterator i = histogram[typ].begin(); i != histogram[typ].end(); ++i) {
                printf("%c\t%d\t%d\n", 'c' + typ, i->second, i->first);
            }
        }
    }
} blah;
#endif


enum {
    log_delta_x_edge = LogTable256[raster_to_aligned.kat<2>() - raster_to_aligned.kat<1>()],
    log_delta_y_edge = LogTable256[raster_to_aligned.kat<16>() - raster_to_aligned.kat<8>()]
};

template<bool has_left, bool has_above, bool has_above_right, BlockType color,
         bool horizontal>
void encode_one_edge(ConstBlockContext context,
                 BoolEncoder& encoder,
                 ProbabilityTables<has_left, has_above, has_above_right, color> & probability_tables,
                 uint8_t num_nonzeros_7x7, uint8_t est_eob,
                 ProbabilityTablesBase& pt) {
    uint8_t num_nonzeros_edge;
    const AlignedBlock &block = context.here();

    if (horizontal) {
        num_nonzeros_edge= (!!block.coefficients_raster(1))
            + (!!block.coefficients_raster(2)) + (!!block.coefficients_raster(3))
            + (!!block.coefficients_raster(4)) + (!!block.coefficients_raster(5))
            + (!!block.coefficients_raster(6)) + (!!block.coefficients_raster(7));
    } else {
        num_nonzeros_edge = (!!block.coefficients_raster(1 * 8))
            + (!!block.coefficients_raster(2 * 8)) + (!!block.coefficients_raster(3 * 8))
            + (!!block.coefficients_raster(4*8)) + (!!block.coefficients_raster(5*8))
            + (!!block.coefficients_raster(6*8)) + (!!block.coefficients_raster(7*8));
    }

    auto prob_edge_eob = horizontal
        ? probability_tables.x_nonzero_counts_8x1(pt, est_eob,
                                                  num_nonzeros_7x7)
        : probability_tables.y_nonzero_counts_1x8(pt, est_eob,
                                                  num_nonzeros_7x7);

    uint8_t aligned_block_offset = raster_to_aligned.at(1);
    unsigned int log_edge_step = log_delta_x_edge;
    uint8_t delta = 1;
    uint8_t zig15offset = 0;
    if (!horizontal) {
        delta = 8;
        log_edge_step = log_delta_y_edge;
        zig15offset = 7;
        aligned_block_offset = raster_to_aligned.at(8);
    }
    int16_t serialized_so_far = 0;
    for (int i= 2; i >=0; --i) {
        int cur_bit = (num_nonzeros_edge & (1 << i)) ? 1 : 0;
        encoder.put(cur_bit, prob_edge_eob.at(i, serialized_so_far));
        serialized_so_far <<= 1;
        serialized_so_far |= cur_bit;
    }

    unsigned int coord = delta;
    for (int lane = 0; lane < 7 && num_nonzeros_edge; ++lane, coord += delta, ++zig15offset) {

        ProbabilityTablesBase::CoefficientContext prior;
        if (ProbabilityTablesBase::MICROVECTORIZE) {
            if (horizontal) {
                prior = probability_tables.update_coefficient_context8_horiz(coord,
                                                                             context,
                                                                             num_nonzeros_edge);
            } else {
                prior = probability_tables.update_coefficient_context8_vert(coord,
                                                                            context,
                                                                            num_nonzeros_edge);
            }
        } else {
            prior = probability_tables.update_coefficient_context8(coord, context, num_nonzeros_edge);
        }
        auto exp_array = probability_tables.exponent_array_x(pt,
                                                             coord,
                                                             zig15offset,
                                                             prior);
        int16_t coef = block.raw_data()[aligned_block_offset + (lane << log_edge_step)];
#ifdef TRACK_HISTOGRAM
            ++histogram[2][coef];
#endif
        uint16_t abs_coef = abs(coef);
        uint8_t length = bit_length(abs_coef);
        auto * exp_branch = exp_array.begin();
        for (unsigned int i = 0; i < length; ++i) {
            encoder.put(1, *exp_branch++);
        }
        encoder.put(0, exp_array.at(length));
        if (coef) {
            uint8_t min_threshold = probability_tables.get_noise_threshold(coord);
            auto &sign_prob = probability_tables.sign_array_8(pt, coord, prior);
            encoder.put(coef >= 0, sign_prob);
            --num_nonzeros_edge;
            if (length > 1){
                int i = length - 2;
                if (i >= min_threshold) {
                    auto thresh_prob = probability_tables.residual_thresh_array(pt,
                                                                                coord,
                                                                                length,
                                                                                prior,
                                                                                min_threshold,
                                                                                probability_tables.get_max_value(coord));
                    uint16_t encoded_so_far = 1;
                    for (; i >= min_threshold; --i) {
                        int cur_bit = (abs_coef & (1 << i)) ? 1 : 0;
                        encoder.put(cur_bit, thresh_prob.at(encoded_so_far));
                        encoded_so_far <<=1;
                        if (cur_bit) {
                            encoded_so_far |=1;
                        }
                    }
#ifdef ANNOTATION_ENABLED
                    probability_tables.residual_thresh_array_annot_update(coord, decoded_so_far >> 2);
#endif
                }
                auto res_prob = probability_tables.residual_noise_array_x(pt, coord, prior);
                for (; i >= 0; --i) {
                    encoder.put((abs_coef & (1 << i)) ? 1 : 0, res_prob.at(i));
                }
            }
        }
    }
}

template<bool has_left, bool has_above, bool has_above_right, BlockType color>
void encode_edge(ConstBlockContext context,
                 BoolEncoder& encoder,
                 ProbabilityTables<has_left, has_above, has_above_right, color> & probability_tables,
                 uint8_t num_nonzeros_7x7, uint8_t eob_x, uint8_t eob_y,
                 ProbabilityTablesBase::CoefficientContext input_prior,
                 ProbabilityTablesBase& pt) {
    encode_one_edge<has_left, has_above, has_above_right, color, true>(context,
                                                                        encoder,
                                                                        probability_tables,
                                                                        num_nonzeros_7x7,
                                                                        eob_x,
                                                                        pt);
    encode_one_edge<has_left, has_above, has_above_right, color, false>(context,
                                                                        encoder,
                                                                        probability_tables,
                                                                        num_nonzeros_7x7,
                                                                        eob_y,
                                                                        pt);
}
// used for debugging
static int k_debug_block[3] = {0, 0, 0};
int total_error = 0;
int total_signed_error = 0;
int amd_err = 0;
int med_err = 0;
int avg_err = 0;
int ori_err = 0;
template <bool has_left, bool has_above, bool has_above_right, BlockType color>
void serialize_tokens(ConstBlockContext context,
                      BoolEncoder& encoder,
                      ProbabilityTables<has_left, has_above, has_above_right, color> & probability_tables,
                      ProbabilityTablesBase &pt)
{
    auto num_nonzeros_prob = probability_tables.nonzero_counts_7x7(pt, context);
    int serialized_so_far = 0;
    uint8_t num_nonzeros_7x7 = context.num_nonzeros_here->num_nonzeros();
#if 0
    fprintf(stderr, "7\t%d\n", (int)block.num_nonzeros_7x7());
    fprintf(stderr, "x\t%d\n", (int)block.num_nonzeros_x());
    fprintf(stderr, "y\t%d\n", (int)block.num_nonzeros_y());
#endif
    for (int index = 5; index >= 0; --index) {
        int cur_bit = (num_nonzeros_7x7 & (1 << index)) ? 1 : 0;
        encoder.put(cur_bit, num_nonzeros_prob.at(index, serialized_so_far));
        serialized_so_far <<= 1;
        serialized_so_far |= cur_bit;
    }
    ProbabilityTablesBase::
        CoefficientContext prior;
    uint8_t eob_x = 0;
    uint8_t eob_y = 0;
    uint8_t num_nonzeros_left_7x7 = num_nonzeros_7x7;

    int avg[4] __attribute__((aligned(16)));
    for (unsigned int zz = 0; zz < 49 && num_nonzeros_left_7x7; ++zz) {
        if ((zz & 3) == 0) {
#ifdef OPTIMIZED_7x7
            probability_tables.compute_aavrg_vec(zz, context.copy(), avg);
#endif
        }

        unsigned int coord = unzigzag49[zz];
        unsigned int b_x = (coord & 7);
        unsigned int b_y = coord >> 3;
        (void)b_x;
        (void)b_y;
        assert(b_x > 0 && b_y > 0 && "this does the DC and the lower 7x7 AC");
        {
            // this should work in all cases but doesn't utilize that the zz is related
            int16_t coef;
#ifdef OPTIMIZED_7x7
            coef = context.here().coef.at(zz + AlignedBlock::AC_7x7_INDEX);
#else
            // this should work in all cases but doesn't utilize that the zz is related
            coef = context.here().coefficients_raster(raster_to_aligned.at(coord));
#endif
            uint16_t abs_coef = abs(coef);
#ifdef TRACK_HISTOGRAM
            ++histogram[0][coef];
#endif
#ifdef OPTIMIZED_7x7
            probability_tables.update_coefficient_context7x7(zz, prior, avg[zz & 3], context.copy(), num_nonzeros_left_7x7);
#else
            probability_tables.update_coefficient_context7x7(coord, zz, prior, context.copy(), num_nonzeros_left_7x7);
#endif
            auto exp_prob = probability_tables.exponent_array_7x7(pt, coord, zz, prior);
            uint8_t length = bit_length(abs_coef);
            for (unsigned int i = 0;i < length; ++i) {
                encoder.put(1, exp_prob.at(i));
            }
            encoder.put(0, exp_prob.at(length));
            if (length != 0) {
                auto &sign_prob = probability_tables.sign_array_7x7(pt, coord, prior);
                encoder.put(coef >= 0 ? 1 : 0, sign_prob);
                --num_nonzeros_left_7x7;
                eob_x = std::max(eob_x, (uint8_t)b_x);
                eob_y = std::max(eob_y, (uint8_t)b_y);
            }
            if (length > 1){
                auto res_prob = probability_tables.residual_noise_array_7x7(pt, coord, prior);
                assert((abs_coef & ( 1 << (length - 1))) && "Biggest bit must be set");
                assert((abs_coef & ( 1 << (length)))==0 && "Beyond Biggest bit must be zero");

                for (int i = length - 2; i >= 0; --i) {
                   encoder.put((abs_coef & (1 << i)), res_prob.at(i));
                }
            }
        }
    }
    encode_edge(context,
                encoder,
                probability_tables,
                num_nonzeros_7x7, eob_x, eob_y,
                prior,
                pt);


    int32_t outp_sans_dc[64];
    prior = probability_tables.get_dc_coefficient_context(context, num_nonzeros_7x7);
    idct(context.here(), ProbabilityTablesBase::quantization_table((int)color), outp_sans_dc, true);
    int predicted_dc = probability_tables.predict_or_unpredict_dc(context, false);
    (void)predicted_dc;
    int uncertainty = 0; // this is how far off our max estimate vs min estimate is
    int adv_predicted_dc = probability_tables.adv_predict_or_unpredict_dc(context,
                                                                          false, 
                                                                          probability_tables.adv_predict_dc_pix(context,
                                                                                                                outp_sans_dc,
                                                                                                                &uncertainty));
    (void)adv_predicted_dc;
    {
        // do DC
        int16_t coef = adv_predicted_dc;
#ifdef TRACK_HISTOGRAM
        ++histogram[1][coef];
#endif
        uint16_t abs_coef = abs(coef);
        uint8_t length = bit_length(abs_coef);
        auto exp_prob = probability_tables.exponent_array_dc(pt, prior, uncertainty);
        for (unsigned int i = 0;i < MAX_EXPONENT; ++i) {
            bool cur_bit = (length != i);
            encoder.put(cur_bit, exp_prob.at(i));
            if (!cur_bit) {
                break;
            }
        }
        if (length != 0) {
            auto &sign_prob = probability_tables.sign_array_dc(pt, prior, uncertainty);
            encoder.put(coef >= 0 ? 1 : 0, sign_prob);
        }
        if (length > 1){
            auto res_prob = probability_tables.residual_array_dc(pt, prior, uncertainty);
            assert((abs_coef & ( 1 << (length - 1))) && "Biggest bit must be set");
            assert((abs_coef & ( 1 << (length)))==0 && "Beyond Biggest bit must be zero");
            for (int i = length - 2; i >= 0; --i) {
                encoder.put((abs_coef & (1 << i)), res_prob.at(i));
            }
        }
    }
    int32_t outp[64];
    idct(context.here(), ProbabilityTablesBase::quantization_table((int)color), outp, false);
    //context.num_nonzeros_here->set_horizontal(outp_sans_dc, context.here().dc());
    ///context.num_nonzeros_here->set_vertical(outp_sans_dc, context.here().dc());
    context.num_nonzeros_here->set_horizontal(outp, 0);
    context.num_nonzeros_here->set_vertical(outp, 0);
    context.num_nonzeros_here->set_dc_residual(adv_predicted_dc);

    if ((!g_threaded) && LeptonDebug::raw_YCbCr[(int)color]) {
        int32_t outp[64];
        idct(context.here(), ProbabilityTablesBase::quantization_table((int)color), outp, false);
        double delta = 0;
        for (int i = 0; i < 64; ++i) {
            delta += outp[i] - outp_sans_dc[i];
            //fprintf (stderr, "%d + %d = %d\n", outp_sans_dc[i], context.here().dc(), outp[i]);
        }
        delta /= 64;
        //fprintf (stderr, "==== %f = %f =?= %d\n", delta, delta * 8, context.here().dc());
        
        int debug_width = LeptonDebug::getDebugWidth((int)color);
        int offset = k_debug_block[(int)color];
        for (int y = 0; y  < 8; ++y) {
            for (int x = 0; x  < 8; ++x) {
                LeptonDebug::raw_YCbCr[(int)color][offset + y * debug_width + x] = std::max(std::min(outp[(y << 3) + x] + 128, 255),0);
            }
        }
        k_debug_block[(int)color] += 8;
        if (k_debug_block[(int)color] % debug_width == 0) {
            k_debug_block[(int)color] += debug_width * 7;
        }
    }
}

template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, false, false, BlockType::Y>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, false, false, BlockType::Cb>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, false, false, BlockType::Cr>&, ProbabilityTablesBase&);

template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, true, false, BlockType::Y>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, true, false, BlockType::Cb>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, true, false, BlockType::Cr>&, ProbabilityTablesBase&);

template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, true, true, BlockType::Y>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, true, true, BlockType::Cb>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<false, true, true, BlockType::Cr>&, ProbabilityTablesBase&);

template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, true, true, BlockType::Y>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, true, true, BlockType::Cb>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, true, true, BlockType::Cr>&, ProbabilityTablesBase&);

template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, true, false, BlockType::Y>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, true, false, BlockType::Cb>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, true, false, BlockType::Cr>&, ProbabilityTablesBase&);

template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, false, false, BlockType::Y>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, false, false, BlockType::Cb>&, ProbabilityTablesBase&);
template void serialize_tokens(ConstBlockContext, BoolEncoder&, ProbabilityTables<true, false, false, BlockType::Cr>&, ProbabilityTablesBase&);



inline void VP8BoolEncoder::put( const bool value, Branch & branch )
{
  put( value, branch.prob() );
  branch.record_obs_and_update(value);
}
