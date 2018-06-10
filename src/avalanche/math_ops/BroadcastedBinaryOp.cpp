#include <clblast.h>
#include <iostream>

#include "avalanche/math_ops/BroadcastedBinaryOp.h"
#include "avalanche/opencl_utils.h"
#include "avalanche/CodeCache.h"
#include "avalanche/casting.h"

namespace avalanche {

constexpr char elem_wise_broadcasted_source[] = {
#include "avalanche/kernels/elem_wise_broadcasted.hex"
};

std::size_t broadcast_size_masks(const Shape &shape1, const Shape &shape2,
                                 std::vector<cl_ulong> &size_mask1,
                                 std::vector<cl_ulong> &size_mask2,
                                 std::vector<cl_ulong> &result_sub_sizes) {
    std::array<avalanche::Shape, 3> aligned_shapes;
    if (shape1.is_scalar() && shape2.is_scalar()) {
        aligned_shapes = std::array<avalanche::Shape, 3>(
            {avalanche::Shape({1}),
             avalanche::Shape({1}),
             avalanche::Shape({1})});
    } else {
        aligned_shapes = avalanche::Shape::align_for_broadcasting(
            shape1, shape2);
    }
    // a scalar value can be interpreted as a vector the shape (1,)
    auto aligned_rank = aligned_shapes[0].rank();
    size_mask1.resize(aligned_rank);
    size_mask2.resize(aligned_rank);
    result_sub_sizes.resize(aligned_rank);
    std::size_t cumprod1 = 1, cumprod2 = 1, cumprod_res = 1;
    for (long i = aligned_rank - 1; i > 0; --i) {
        cumprod1 *= aligned_shapes[0].dims()[i];
        cumprod2 *= aligned_shapes[1].dims()[i];
        cumprod_res *= aligned_shapes[2].dims()[i];
        size_mask1[i - 1] = aligned_shapes[0].dims()[i - 1] == 1 ? 0 : cumprod1;
        size_mask2[i - 1] = aligned_shapes[1].dims()[i - 1] == 1 ? 0 : cumprod2;
        result_sub_sizes[i - 1] = cumprod_res;
    }
    size_mask1[aligned_rank - 1] = aligned_shapes[0].dim(-1) == 1 ? 0 : 1;
    size_mask2[aligned_rank - 1] = aligned_shapes[1].dim(-1) == 1 ? 0 : 1;
    return aligned_shapes[2].size();
}

/**
 * Creates a full list of dimensions being added to the argument's
 *  shape during broadcasting. Useful for later calculation of derivatives.
 */
std::vector<ShapeDim> dims_difference(const Shape &aligned_shape,
                                      const Shape &result_shape) {
    std::vector<ShapeDim> result;
    if (aligned_shape.rank() != result_shape.rank()) {
        throw std::invalid_argument("The shapes must be aligned!");
    }
    for (ShapeDim i = 0; i < aligned_shape.rank(); ++i) {
        if (aligned_shape.dims()[i] != result_shape.dims()[i]) {
            result.push_back(i);
        }
    }
    return result;
}

BroadcastedBinaryOp::BroadcastedBinaryOp(
        const avalanche::NodeRef &left,
        const avalanche::NodeRef &right) {
    if (left->dtype() != right->dtype()) {
        throw std::invalid_argument(
            "You cannot sum values of different types");
    }
    result_dtype = left->dtype();
    Shape::align_for_broadcasting(
        left->shape(), right->shape(),
        aligned_shape_left, aligned_shape_right, result_shape);
    broadcast_size_masks(
        left->shape(), right->shape(),
        left_size_mask, right_size_mask, result_sub_sizes);
    left_vs_result_shape_diff = dims_difference(
        aligned_shape_left, result_shape);
    right_vs_result_shape_diff = dims_difference(
        aligned_shape_right, result_shape);
}


MultiArrayRef avalanche::BroadcastedBinaryOp::forward(
        const MultiArrayRef &v1,
        const MultiArrayRef &v2) const {
    // At this point we assume that every aspect of both arrays have already
    // been checked by the constructor, so there's nothing to be worrying about
    // Also at this point both arrays v1 and v2 may still be calculating,
    // so meanwhile we prepare and upload some other data
    auto pool = v1->buffer_unsafe()->pool();
    auto queue = pool->cl_queue();
    auto left_mask_buffer = pool->reserve_buffer_for_vector(left_size_mask);
    left_mask_buffer->set_label(__func__, __LINE__);
    auto right_mask_buffer = pool->reserve_buffer_for_vector(right_size_mask);
    right_mask_buffer->set_label(__func__, __LINE__);
    auto result_sizes_buffer = pool->reserve_buffer_for_vector(result_sub_sizes);
    result_sizes_buffer->set_label(__func__, __LINE__);
    // You kinda have a problem here
    // The problem is that you use both the cl::Event directly when you schedule
    // something for execution, but you also rely on callbacks for some extra
    // processing. The problem is that you cannot guarantee when the callback
    // will arrive. And sometimes it happens after the actual Buffer has already
    // been destroyed!

    // Here we send the data to the buffers, plus create the full
    // list of operations we need to be done before we can start
    // performing computations.
//    for (auto i: left_size_mask) { std::cout << i << ", "; } std::cout << std::endl;
//    for (auto i: right_size_mask) { std::cout << i << ", "; } std::cout << std::endl;
//    for (auto i: result_sub_sizes) { std::cout << i << ", "; } std::cout << std::endl;
    auto data_are_ready = make_event_list(
        {left_mask_buffer->write_from_vector(left_size_mask),
         right_mask_buffer->write_from_vector(right_size_mask),
         result_sizes_buffer->write_from_vector(result_sub_sizes),
         v1->buffer_unsafe()->completion_event(),
         v2->buffer_unsafe()->completion_event()});
    auto result = pool->make_array(result_shape, result_dtype);
    result->set_label(std::string(kernel_op_name()) + " at " + __func__, __LINE__);
    // To keep the buffers alive until the computation is done
    result->add_dependencies(
        {left_mask_buffer, right_mask_buffer, result_sizes_buffer});
    result->add_dependencies({v1, v2});
    // cl::Event::waitForEvents(data_are_ready);
    // The main job
    const std::string source(
        elem_wise_broadcasted_source,
        sizeof(elem_wise_broadcasted_source));
    auto program = CodeCache::get_default().get_program(
        pool->cl_context(), queue,
        "elem_wise_broadcasted", source, "");
    using Buf = const cl::Buffer&;
    cl::KernelFunctor<Buf, Buf, Buf, Buf, Buf, Buf, cl_ulong, cl_int>
        kernel_functor(program, cached_kernel_name());
    const auto result_size = result_shape.size();
    const std::size_t work_group_size = 64;
    const auto work_items = make_divisible_by(work_group_size, result_size);
    cl_int mask_rank = left_size_mask.size();
//    std::cout << "Mask rank: " << mask_rank << std::endl;
    cl::Event result_event = kernel_functor(
        cl::EnqueueArgs(queue,
                        data_are_ready,
                        cl::NDRange(work_items),
                        cl::NDRange(work_group_size)),
        v1->cl_buffer_unsafe(),
        v2->cl_buffer_unsafe(),
        result->cl_buffer_unsafe(),
        left_mask_buffer->cl_buffer_unsafe(),
        right_mask_buffer->cl_buffer_unsafe(),
        result_sizes_buffer->cl_buffer_unsafe(),
        static_cast<cl_ulong>(result_size),
        mask_rank);
    // Let us know when everything is done by marking the resulting array
    // as "complete" (ready)
    result->set_completion_event(result_event);
    return result;
}

} // namespace