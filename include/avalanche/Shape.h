#ifndef AVALANCHE_SHAPE_H
#define AVALANCHE_SHAPE_H

#include <vector>
#include <string>
#include <array>

namespace avalanche {

using ShapeDim = long long;

class Shape {

public:
    Shape() {}
//    explicit Shape(std::vector<ShapeDim> dims);
    Shape(const std::vector<ShapeDim> &dims);
    Shape(std::initializer_list<ShapeDim> dims);
    Shape(const Shape& shape) :_dims{shape._dims} {}
    Shape& operator=(const Shape& shape) { _dims = shape._dims; return *this; }
    Shape(Shape&& shape) noexcept :_dims{std::move(shape._dims)} {}
    const std::vector<ShapeDim> &dims() const { return _dims; }
    std::size_t dim_real_index(ShapeDim dim) const;
    ShapeDim dim(ShapeDim index) const;
    std::size_t size() const;
    std::size_t rank() const { return _dims.size(); }
    bool is_scalar() const { return _dims.empty(); }
    Shape reshape(const std::vector<ShapeDim> &dims) const;
    ShapeDim operator[](ShapeDim index) const;
    bool operator==(const Shape &other) const { return other._dims == _dims; }
    bool operator!=(const Shape &other) const { return other._dims != _dims; }
    std::string to_string() const;

    /**
     * Unifies two shapes so they could be used in element-wise broadcasted
     * operations. The rules are the same as in numpy. For example, you can
     * add a matrix (2, 3) and a vector (3), getting a new matrix (2, 3).
     * In the process, the vector is going to be broadcasted to a equivalent
     * matrix (1, 3) * so that the rank would be identical.
     * This function checks if two shapes are compatible for broadcasting
     * and if they are, outputs three shapes:
     * 1. How the first shape should look in order to be compatible
     *    (broadcastable).
     * 2. How the second shape should look in order to be compatible.
     * 3. The shape of the final result of element-wise operation (if possible).
     */
    static std::array<Shape, 3> align_for_broadcasting(
        const Shape &shape1, const Shape &shape2);
    static void align_for_broadcasting(
        const Shape &shape1, const Shape &shape2,
        Shape &shape1_aligned, Shape &shape2_aligned,
        Shape &result_shape);
    static std::string dims_to_string(const std::vector<ShapeDim> &dims);

private:
    std::vector<ShapeDim> _dims;
};

} // namespace


#endif //AVALANCHE_SHAPE_H