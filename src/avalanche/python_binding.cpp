#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "avalanche/terminal_nodes.h"
#include "avalanche/nodes.h"
#include "avalanche/Shape.h"
#include "avalanche/Executor.h"

namespace py = pybind11;

namespace avalanche {

class PyBaseNode : public BaseNode {
public:
    using BaseNode::BaseNode;

    MultiArrayRef
    eval(Context &context, ExecutionCache &cache) const override {
        PYBIND11_OVERLOAD_PURE(
            MultiArrayRef,
            BaseNode,
            eval,
            context, cache);
    }

    const NodeRef apply_chain_rule(
        const NodeRef &wrt_input,
        const NodeRef &d_target_wrt_this,
        const NodeRefList &all_inputs) const override {
        PYBIND11_OVERLOAD_PURE(
            NodeRef,
            BaseNode,
            apply_chain_rule,
            wrt_input, d_target_wrt_this, all_inputs);
    };

    std::string to_string() const override {
        PYBIND11_OVERLOAD_PURE(
            std::string,
            BaseNode,
            to_string);
    };

    NodeRefList inputs() const override {
        PYBIND11_OVERLOAD_PURE(
            NodeRefList,
            BaseNode,
            inputs);
    };

    bool is_variable() const override {
        PYBIND11_OVERLOAD_PURE(
            bool,
            BaseNode,
            is_variable);
    }

    bool is_scalar_variable() const override {
        PYBIND11_OVERLOAD_PURE(
            bool,
            BaseNode,
            is_scalar_variable);
    }
};


template<typename Op>
NodeRef SimpleBinaryOp(const NodeRef &a, const NodeRef &b) {
    return std::static_pointer_cast<BaseNode>(
        std::make_shared<BinaryOp<Op>>(a, b));
}

NodeRef matmul(const NodeRef &a, const NodeRef &b,
                   const bool transpose_left = false,
                   const bool transpose_right = false) {
    return std::static_pointer_cast<BaseNode>(
        std::make_shared<BinaryOp<MatMul>>(
            a, b, transpose_left, transpose_right));
}

ArrayType dtype_to_avalanche_array_type(const py::dtype &dtype) {
    ArrayType array_type;
    switch (dtype.kind()) {
        case 'f':
            // identify dtype
            switch (dtype.itemsize()) {
                case 2:
                    array_type = ArrayType::float16;
                    break;
                case 4:
                    array_type = ArrayType::float32;
                    break;
                case 8:
                    array_type = ArrayType::float64;
                    break;
                default:
                    throw std::invalid_argument("Unsupported float type size");
            }
            break;
        case 'i':
            switch (dtype.itemsize()) {
                case 1:
                    array_type = ArrayType::int8;
                    break;
                case 2:
                    array_type = ArrayType::int16;
                    break;
                case 4:
                    array_type = ArrayType::int32;
                    break;
                case 8:
                    array_type = ArrayType::int64;
                    break;
                default:
                    throw std::invalid_argument("Unsupported int type size");
            }
            break;
        default:
            throw std::invalid_argument("Unsupported array kind");
    }
    return array_type;
}


using ContextInitArrayType = float;
using ContextInitArray = py::array_t<
    ContextInitArrayType,
    py::array::forcecast | py::array::c_style>;

/** Initializes context with automatic conversion from given numpy array
 * to whatever the node requires */
template <typename T>
MultiArrayRef init_context_with_cast(
        const NodeRef &node, ContextRef &context, ContextInitArray &data) {
    std::vector<T> tmp_copy(static_cast<std::size_t>(data.size()));
    py::buffer_info info = data.request();
    std::copy((const ContextInitArrayType *)info.ptr,
              ((const ContextInitArrayType *)info.ptr) + data.size(),
              tmp_copy.begin());
    std::vector<ShapeDim> dims(info.shape.size());
    std::copy(info.shape.begin(), info.shape.end(), dims.begin());
    return context->init(node, tmp_copy, Shape(dims));
}

/**
 * Initializes context, specialized version for case when dtype of the given
 * numpy array exactly matches the one of the node.
 */
template<>
MultiArrayRef init_context_with_cast<ContextInitArrayType >(
    const NodeRef &node, ContextRef &context, ContextInitArray &data) {
    py::buffer_info info = data.request();
    std::vector<ShapeDim> dims(info.shape.size());
    std::copy(info.shape.begin(), info.shape.end(), dims.begin());
    return context->init(
        node, info.ptr, static_cast<std::size_t>(data.nbytes()),
        dtype_of_static_type<ContextInitArrayType>, Shape(dims));
}

ARRAY_DTYPE_SWITCH_FUNCTION(switch_init_context_with_cast, init_context_with_cast, MultiArrayRef,)

MultiArrayRef
init_context(ContextRef &context, const NodeRef &node, ContextInitArray data) {
    ArrayType required_array_type = node->dtype();
    return switch_init_context_with_cast(required_array_type, node, context, data);
}

template <typename T>
py::array array_to_numpy_template(MultiArrayRef &array) {
    std::vector<ssize_t> dims(array->shape().rank());
    auto &array_dims = array->shape().dims();
    std::copy(array_dims.begin(), array_dims.end(), dims.begin());
    py::array_t<T, py::array::c_style> result(dims);
    auto info = result.request(true);
    array->wait_until_ready();
    auto reading_is_done = (
        array->buffer_when_ready()
             ->read_data(info.ptr,
                         static_cast<std::size_t>(info.size * info.itemsize)));
    reading_is_done.wait();
    return result;
}

ARRAY_DTYPE_SWITCH_FUNCTION(array_to_numpy_switch, array_to_numpy_template, py::array,)


py::array array_to_numpy(MultiArrayRef &array) {
    ArrayType required_dtype = array->dtype();
    return array_to_numpy_switch(required_dtype, array);
}

PYBIND11_MODULE(pyvalanche, m) {
    m.doc() = R"pbdoc(
        Avalanche ML framework
        -----------------------
        .. currentmodule:: pyvalanche
        .. autosummary::
           :toctree: _generate
    )pbdoc";

    py::class_<Shape>(m, "Shape")
        .def(py::init<const std::vector<ShapeDim> &>())
        .def("__str__", &Shape::to_string)
        .def("dim", &Shape::dim)
        .def("dims", &Shape::dim)
        .def_property_readonly("size", &Shape::size)
        .def_property_readonly("rank", &Shape::rank)
        .def("is_scalar", &Shape::is_scalar)
        .def("reshape", &Shape::reshape)
        .def("__eq__", &Shape::operator==)
        .def("__ne__", &Shape::operator!=)
        .def("__getitem__", &Shape::operator[]);

    py::enum_<ArrayType>(m, "ArrayType")
        .value("int8", ArrayType::int8)
        .value("int16", ArrayType::int16)
        .value("int32", ArrayType::int32)
        .value("int64", ArrayType::int64)
        .value("float16", ArrayType::float16)
        .value("float32", ArrayType::float32)
        .value("float64", ArrayType::float64)
        .export_values();

    py::class_<BaseNode, NodeRef>(m, "BaseNode")
        .def("__str__", &BaseNode::to_string)
        .def("__repr__", &BaseNode::repr)
        .def("inputs", &BaseNode::inputs)
        .def("dtype", &BaseNode::dtype)
        .def_property_readonly("shape", &BaseNode::shape);

    py::class_<MultiArray, MultiArrayRef>(m, "MultiArray")
        .def("asnumpy", &array_to_numpy);

    py::class_<Context, ContextRef>(m, "Context")
        .def_static("make_for_device", &Context::make_for_device)
        .def("init", &init_context)
        .def("eval", &Context::eval);

    py::class_<GradTable>(m, "GradTable");
    py::class_<Executor>(m, "Executor")
        .def(py::init<const ContextRef&, const NodeRefList&>())
        .def("run", &Executor::run);

    m.def("build_back_propagation_graph", &build_back_propagation_graph);

    m.def("variable", &Variable::pymake,
          R"pbdoc(
      Creates a new variable
      )pbdoc");

    py::module ops = m.def_submodule("ops", "Available operations");

#define REDUCE_ARGS \
        py::arg_v("node", "input tensor"), \
        py::arg_v("reduce_axis", std::vector<ShapeDim>(), "axis to reduce"), \
        py::arg_v("keep_dims", false, "Keep reduced dimensions")

    ops
        .def("relu", &FU<ReLU>)
        .def("tanh", &FU<Tanh>)
        .def("sigmoid", &FU<Sigmoid>)
        .def("log", &FU<Log>)
        .def("exp", &FU<Exp>)
        .def("plus", &SimpleBinaryOp<Plus>,
             "Elem-wise addition with broadcasting")
        .def("minus", &SimpleBinaryOp<Minus>,
             "Elem-wise subtraction with broadcasting")
        .def("divide", &SimpleBinaryOp<Divide>,
             "Elem-wise division with broadcasting")
        .def("multiply", &SimpleBinaryOp<Multiply>,
             "Elem-wise multiplication with broadcasting")
        .def("matmul", &matmul,
             py::arg_v("a", "Left matrix"),
             py::arg_v("b", "Right matrix"),
             py::arg_v("transpose_left", false,
                       "set to True if the first matrix needs "
                       "to be transposed before multiplication"),
             py::arg_v("transpose_right", false,
                       "set to True if the second matrix needs "
                       "to be transposed before multiplication"))
        .def("softmax", &softmax,
             py::arg_v("node", "Input tensor"),
             py::arg_v("axis", -1, "Dimension to perform on"))
        .def("reshape", &FU<Reshape, const Shape&>)
        .def("reduce_sum", &FU<ReduceSum, std::vector<ShapeDim>, bool>, REDUCE_ARGS)
        .def("reduce_mean", &FU<ReduceMean, std::vector<ShapeDim>, bool>, REDUCE_ARGS);
//    .def("plus", [](const NodeRef &left, const NodeRef &right) -> NodeRef { return std::static_pointer_cast<BaseNode>(std::make_shared<Plus>(left, right)); });
//    .def("matmul", &F<MatMul>);
#undef REDUCE_ARGS

//m.def("add", &add, R"pbdoc(
//        Add two numbers
//        Some other explanation about the add function.
//    )pbdoc");
//
//m.def("subtract", [](int i, int j) { return i - j; }, R"pbdoc(
//        Subtract two numbers
//        Some other explanation about the subtract function.
//    )pbdoc");

#ifdef VERSION_INFO
    m.attr("__version__") = VERSION_INFO;
#else
    m.attr("__version__") = "dev";
#endif
}

} // namespace