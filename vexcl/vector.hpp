#ifndef VEXCL_VECTOR_HPP
#define VEXCL_VECTOR_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   vector.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  OpenCL device vector.
 */

#ifdef WIN32
#  pragma warning(disable : 4267 4290)
#  define NOMINMAX
#endif

#define __CL_ENABLE_EXCEPTIONS

#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <CL/cl.hpp>
#include <vexcl/util.hpp>
#include <vexcl/profiler.hpp>

/// OpenCL convenience utilities.
namespace vex {

template<class T, typename column_t> struct SpMV;
template <class Expr, typename T, typename column_t> struct ExSpMV;

/// Base class for a member of an expression.
/**
 * Each vector expression results in a single kernel. Name of the kernel,
 * parameter list and the body are formed automatically with the help of
 * members of the expression. This class is an abstract interface which any
 * expression member should implement.
 */
struct expression {
    static const bool is_expression = true;

    /// Preamble.
    /**
     * An expression might need to put something prior the kernel definition.
     * This could be a typedef, a helper function definition or anything else.
     * Most expressions don't need it and so default empty function is
     * provided.
     * \param os   Output stream which holds kernel source.
     * \param name Name of current node in an expression tree. Should be used
     *		   as prefix when naming any objects created to escape
     *		   possible ambiguities with other nodes.
     */
    virtual void preamble(std::ostream &os, std::string name) const {
    }

    /// Kernel name.
    /**
     * Name of the kernel is formed by its members through calls to their
     * kernel_name() functions. For example, expression
     * \code
     *   x = 3 * y + z;
     * \endcode
     * will result in kernel named "ptcvv" (plus times constant vector vector;
     * polish notation is used).
     * This naming scheme is not strictly necessary, as each expression
     * template holds its own cl::Program object and no ambiguity is possible.
     * But it helps when you profiling your program performance.
     */
    virtual std::string kernel_name() const = 0;

    /// Kernel parameter list.
    /**
     * Each terminal expression should output type and name of kernel
     * parameters it needs here.
     * \param os   Output stream which holds kernel source.
     * \param name Name of current node in an expression tree. Should be used
     *             directly or as a prefix to form parameter name(s).
     */
    virtual void kernel_prm(std::ostream &os, std::string name) const = 0;

    /// Kernel arguments.
    /**
     * This function is called at the time of actual kernel launch.
     * Each terminal expression should set kernel arguments for the parameters
     * it needs at specified position. Position should be incremented
     * afterwards.
     * \param k      OpenCL kernel that is being prepared to launch.
     * \param devnum Number of queue in queue list for which the kernel is
     *               launched.
     * \param pos    Current position in parameter stack.
     */
    virtual void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const = 0;

    /// Kernel body.
    /**
     * The actual expression which forms the kernel body.
     * \param os   Output stream which holds kernel source.
     * \param name Name of current node in an expression tree. Should be used
     *             directly or as a prefix to form parameter name(s).
     */
    virtual void kernel_expr(std::ostream &os, std::string name) const = 0;

    /// Size of vectors forming the expression.
    /**
     * \param dev Position in active queue list for which to return the size.
     */
    virtual size_t part_size(uint dev) const = 0;

    virtual ~expression() {}
};

/// Default kernel generation helper.
/**
 * Works on top of classes inheriting expression interface;
 */
template <class T, class Enable = void>
struct KernelGenerator {
    KernelGenerator(const T &value) : value(value) {}

    void preamble(std::ostream &os, std::string name) const {
	value.preamble(os, name);
    }

    std::string kernel_name() const {
	return value.kernel_name();
    }

    void kernel_prm(std::ostream &os, std::string name) const {
	value.kernel_prm(os, name);
    }

    void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
	value.kernel_args(k, devnum, pos);
    }

    void kernel_expr(std::ostream &os, std::string name) const {
	value.kernel_expr(os, name);
    }

    size_t part_size(uint dev) const {
	return value.part_size(dev);
    }

    private:
	const T &value;
};

/// Kernel generation helper for arithmetic types.
template <typename T>
struct KernelGenerator<T, typename std::enable_if<std::is_arithmetic<T>::value>::type> {
    KernelGenerator(const T &value) : value(value) {}

    void preamble(std::ostream &os, std::string name) const {}

    std::string kernel_name() const {
	return "c";
    }

    void kernel_expr(std::ostream &os, std::string name) const {
	os << name;
    }

    void kernel_prm(std::ostream &os, std::string name) const {
	os << ",\n\t" << type_name<T>() << " " << name;
    }

    void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
	k.setArg(pos++, value);
    }

    size_t part_size(uint dev) const {
	return 0;
    }

    private:
	const T &value;
};

template <class T, class Enable = void>
struct valid_expression {
    static const bool value = false;
};

template <typename T>
struct valid_expression<T, typename std::enable_if<T::is_expression>::type> {
    static const bool value = true;
};

template <typename T>
struct valid_expression<T, typename std::enable_if<std::is_arithmetic<T>::value>::type> {
    static const bool value = true;
};

/// Device vector.
template<class T>
class vector : public expression {
    public:
	/// Proxy class.
	/**
	 * Instances of this class are returned from vector::operator[]. These
	 * may be used to read or write single element of a vector, although
	 * this operations are too expensive to be used extensively and should
	 * be reserved for debugging purposes.
	 */
	class element {
	    public:
		/// Read associated element of a vector.
		operator T() const {
		    T val;
		    queue.enqueueReadBuffer(
			    buf, CL_TRUE,
			    index * sizeof(T), sizeof(T),
			    &val
			    );
		    return val;
		}

		/// Write associated element of a vector.
		T operator=(T val) {
		    queue.enqueueWriteBuffer(
			    buf, CL_TRUE,
			    index * sizeof(T), sizeof(T),
			    &val
			    );
		    return val;
		}
	    private:
		element(const cl::CommandQueue &q, cl::Buffer b, size_t i)
		    : queue(q), buf(b), index(i) {}

		const cl::CommandQueue  &queue;
		cl::Buffer              buf;
		const size_t            index;

		friend class vector;
	};

	/// Iterator class.
	/**
	 * This class may in principle be used with standard template library,
	 * although its main purpose is range specification for vector copy
	 * operations.
	 */
	template <class vector_type, class element_type>
	class iterator_type
	    : public std::iterator<std::random_access_iterator_tag, T>
	{
	    public:
		static const bool device_iterator = true;

		element_type operator*() const {
		    return element_type(
			    vec.queue[part], vec.buf[part],
			    pos - vec.part[part]
			    );
		}

		iterator_type& operator++() {
		    pos++;
		    while (pos >= vec.part[part + 1] && part < vec.nparts())
			part++;
		    return *this;
		}

		iterator_type operator+(ptrdiff_t d) const {
		    return iterator_type(vec, pos + d);
		}

		ptrdiff_t operator-(iterator_type it) const {
		    return pos - it.pos;
		}

		bool operator==(const iterator_type &it) const {
		    return pos == it.pos;
		}

		bool operator!=(const iterator_type &it) const {
		    return pos != it.pos;
		}

		vector_type &vec;
		size_t  pos;
		size_t  part;

	    private:
		iterator_type(vector_type &vec, size_t pos)
		    : vec(vec), pos(pos), part(0)
		{
		    if (!vec.part.empty())
			while(pos >= vec.part[part + 1] && part < vec.nparts())
			    part++;
		}

		friend class vector;
	};

	typedef iterator_type<vector, element> iterator;
	typedef iterator_type<const vector, const element> const_iterator;

	/// Empty constructor.
	vector() {}

	/// Copy constructor.
	vector(const vector &v)
	    : queue(v.queue()), part(v.part),
	      buf(queue.size()), event(queue.size())
	{
	    if (size()) allocate_buffers(CL_MEM_READ_WRITE, 0);
	    *this = v;
	}

	/// Copy host data to the new buffer.
	vector(const std::vector<cl::CommandQueue> &queue,
		size_t size, const T *host = 0,
		cl_mem_flags flags = CL_MEM_READ_WRITE
	      ) : queue(queue), part(vex::partition(size, queue)),
	          buf(queue.size()), event(queue.size())
	{
	    if (size) allocate_buffers(flags, host);
	}

	/// Copy host data to the new buffer.
	vector(const std::vector<cl::CommandQueue> &queue,
		const std::vector<T> &host,
		cl_mem_flags flags = CL_MEM_READ_WRITE
	      ) : queue(queue), part(vex::partition(host.size(), queue)),
		  buf(queue.size()), event(queue.size())
	{
	    if (!host.empty()) allocate_buffers(flags, host.data());
	}

	/// Move constructor
	vector(vector &&v)
	    : queue(std::move(v.queue)), part(std::move(v.part)),
	      buf(std::move(v.buf)), event(std::move(v.event))
	{
	}

	/// Move assignment
	const vector& operator=(vector &&v) {
	    swap(v);
	    return *this;
	}

	/// Swap function.
	void swap(vector &v) {
	    std::swap(queue,   v.queue);
	    std::swap(part,    v.part);
	    std::swap(buf,     v.buf);
	    std::swap(event,   v.event);
	}

	/// Resize vector.
	void resize(const vector &v, cl_mem_flags flags = CL_MEM_READ_WRITE)
	{
	    *this = std::move(vector(v.queue, v.size(), 0, flags));
	    *this = v;
	}

	/// Resize vector.
	void resize(const std::vector<cl::CommandQueue> &queue,
		size_t size, const T *host = 0,
		cl_mem_flags flags = CL_MEM_READ_WRITE
		)
	{
	    *this = std::move(vector(queue, size, host, flags));
	}

	/// Resize vector.
	void resize(const std::vector<cl::CommandQueue> &queue,
		const std::vector<T> &host,
		cl_mem_flags flags = CL_MEM_READ_WRITE
	      )
	{
	    *this = std::move(vector(queue, host, flags));
	}

	/// Return cl::Buffer object located on a given device.
	cl::Buffer operator()(uint d = 0) const {
	    return buf[d];
	}

	/// Const iterator to beginning.
	const_iterator begin() const {
	    return const_iterator(*this, 0);
	}

	/// Const iterator to end.
	const_iterator end() const {
	    return const_iterator(*this, size());
	}

	/// Iterator to beginning.
	iterator begin() {
	    return iterator(*this, 0);
	}

	/// Iterator to end.
	iterator end() {
	    return iterator(*this, size());
	}

	/// Access element.
	const element operator[](size_t index) const {
	    uint d = 0;
	    while(index >= part[d + 1] && d < nparts()) d++;
	    return element(queue[d], buf[d], index - part[d]);
	}

	/// Access element.
	element operator[](size_t index) {
	    uint d = 0;
	    while(index >= part[d + 1] && d < nparts()) d++;
	    return element(queue[d], buf[d], index - part[d]);
	}

	/// Return size .
	size_t size() const {
	    return part.empty() ? 0 : part.back();
	}

	/// Return number of parts (devices).
	uint nparts() const {
	    return queue.size();
	}

	/// Return size of part on a given device.
	size_t part_size(uint d) const {
	    return part[d + 1] - part[d];
	}

	/// Return reference to vector's queue list
	const std::vector<cl::CommandQueue>& queue_list() const {
	    return queue;
	}

	/// Copies data from device vector.
	const vector& operator=(const vector &x) {
	    if (&x != this) {
		for(uint d = 0; d < queue.size(); d++)
		    if (size_t psize = part[d + 1] - part[d]) {
			queue[d].enqueueCopyBuffer(x.buf[d], buf[d], 0, 0,
				psize * sizeof(T));
		    }
	    }

	    return *this;
	}

	/** \name Expression assignments.
	 * @{
	 * The appropriate kernel is compiled first time the assignment is
	 * made. Vectors participating in expression should have same number of
	 * parts; corresponding parts of the vectors should reside on the same
	 * compute devices.
	 */
	template <class Expr>
	    const vector& operator=(const Expr &expr) {
		KernelGenerator<Expr> kgen(expr);

		for(auto q = queue.begin(); q != queue.end(); q++) {
		    cl::Context context = q->getInfo<CL_QUEUE_CONTEXT>();

		    if (!exdata<Expr>::compiled[context()]) {
			std::vector<cl::Device> device = context.getInfo<CL_CONTEXT_DEVICES>();

			bool device_is_cpu = (
				device[0].getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU
				);

			std::ostringstream kernel;

			std::string kernel_name = kgen.kernel_name();

			kernel << standard_kernel_header;

			kgen.preamble(kernel, "prm");

			kernel <<
			    "kernel void " << kernel_name << "(\n"
			    "\t" << type_name<size_t>() << " n,\n"
			    "\tglobal " << type_name<T>() << " *res";

			kgen.kernel_prm(kernel, "prm");

			kernel <<
			    "\n\t)\n{\n"
			    "\tsize_t i = get_global_id(0);\n";
			if (device_is_cpu) {
			    kernel <<
				"\tif (i < n) {\n"
				"\t\tres[i] = ";
			} else {
			    kernel <<
				"\tsize_t grid_size = get_num_groups(0) * get_local_size(0);\n"
				"\twhile (i < n) {\n"
				"\t\tres[i] = ";
			}

			kgen.kernel_expr(kernel, "prm");

			if (device_is_cpu) {
			    kernel <<
				";\n"
				"\t}\n"
				"}" << std::endl;
			} else {
			    kernel <<
				";\n"
				"\t\ti += grid_size;\n"
				"\t}\n"
				"}" << std::endl;
			}

#ifdef VEX_SHOW_KERNELS
			std::cout << kernel.str() << std::endl;
#endif

			auto program = build_sources(context, kernel.str());

			exdata<Expr>::kernel[context()]   = cl::Kernel(program, kernel_name.c_str());
			exdata<Expr>::compiled[context()] = true;
			exdata<Expr>::wgsize[context()]   = kernel_workgroup_size(
				exdata<Expr>::kernel[context()], device);
		    }
		}

		for(uint d = 0; d < queue.size(); d++) {
		    if (size_t psize = part[d + 1] - part[d]) {
			cl::Context context = queue[d].getInfo<CL_QUEUE_CONTEXT>();
			cl::Device  device  = queue[d].getInfo<CL_QUEUE_DEVICE>();

			size_t g_size = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU ?
			    alignup(psize, exdata<Expr>::wgsize[context()]) :
			    device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() * exdata<Expr>::wgsize[context()] * 4;

			uint pos = 0;
			exdata<Expr>::kernel[context()].setArg(pos++, psize);
			exdata<Expr>::kernel[context()].setArg(pos++, buf[d]);
			kgen.kernel_args(exdata<Expr>::kernel[context()], d, pos);

			queue[d].enqueueNDRangeKernel(
				exdata<Expr>::kernel[context()],
				cl::NullRange,
				g_size, exdata<Expr>::wgsize[context()]
				);
		    }
		}

		return *this;
	    }

	template <class Expr>
	const vector& operator+=(const Expr &expr) {
	    return *this = *this + expr;
	}

	template <class Expr>
	const vector& operator*=(const Expr &expr) {
	    return *this = *this * expr;
	}

	template <class Expr>
	const vector& operator/=(const Expr &expr) {
	    return *this = *this / expr;
	}

	template <class Expr>
	const vector& operator-=(const Expr &expr) {
	    return *this = *this - expr;
	}

	template <class Expr, typename column_t>
	const vector& operator=(const ExSpMV<Expr,T,column_t> &xmv);

	template <typename column_t>
	const vector& operator= (const SpMV<T,column_t> &spmv);

	template <typename column_t>
	const vector& operator+=(const SpMV<T,column_t> &spmv);

	template <typename column_t>
	const vector& operator-=(const SpMV<T,column_t> &spmv);
	/// @}

	/** \name Service methods used for kernel generation.
	 * @{
	 */
	std::string kernel_name() const {
	    return "v";
	}

	void kernel_expr(std::ostream &os, std::string name) const {
	    os << name << "[i]";
	}

	void kernel_prm(std::ostream &os, std::string name) const {
	    os << ",\n\tglobal " << type_name<T>() << " *" << name;
	}

	void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
	    k.setArg(pos++, buf[devnum]);
	}
	/// @}

	/// Copy data from host buffer to device(s).
	void write_data(size_t offset, size_t size, const T *hostptr, cl_bool blocking) {
	    if (!size) return;

	    for(uint d = 0; d < queue.size(); d++) {
		size_t start = std::max(offset,        part[d]);
		size_t stop  = std::min(offset + size, part[d + 1]);

		if (stop <= start) continue;

		queue[d].enqueueWriteBuffer(buf[d], CL_FALSE,
			sizeof(T) * (start - part[d]),
			sizeof(T) * (stop - start),
			hostptr + start - offset,
			0, &event[d]
			);
	    }

	    if (blocking)
		for(size_t d = 0; d < queue.size(); d++) {
		    size_t start = std::max(offset,        part[d]);
		    size_t stop  = std::min(offset + size, part[d + 1]);

		    if (start < stop) event[d].wait();
		}
	}

	/// Copy data from device(s) to host buffer .
	void read_data(size_t offset, size_t size, T *hostptr, cl_bool blocking) const {
	    if (!size) return;

	    for(uint d = 0; d < queue.size(); d++) {
		size_t start = std::max(offset,        part[d]);
		size_t stop  = std::min(offset + size, part[d + 1]);

		if (stop <= start) continue;

		queue[d].enqueueReadBuffer(buf[d], CL_FALSE,
			sizeof(T) * (start - part[d]),
			sizeof(T) * (stop - start),
			hostptr + start - offset,
			0, &event[d]
			);
	    }

	    if (blocking)
		for(uint d = 0; d < queue.size(); d++) {
		    size_t start = std::max(offset,        part[d]);
		    size_t stop  = std::min(offset + size, part[d + 1]);

		    if (start < stop) event[d].wait();
		}
	}
    private:
	template <class Expr>
	struct exdata {
	    static std::map<cl_context,bool>       compiled;
	    static std::map<cl_context,cl::Kernel> kernel;
	    static std::map<cl_context,size_t>     wgsize;
	};

	std::vector<cl::CommandQueue>	queue;
	std::vector<size_t>             part;
	std::vector<cl::Buffer>		buf;
	mutable std::vector<cl::Event>  event;

	void allocate_buffers(cl_mem_flags flags, const T *hostptr) {
	    for(uint d = 0; d < queue.size(); d++) {
		if (size_t psize = part[d + 1] - part[d]) {
		    cl::Context context = queue[d].getInfo<CL_QUEUE_CONTEXT>();

		    buf[d] = cl::Buffer(context, flags, psize * sizeof(T));
		}
	    }
	    if (hostptr) write_data(0, size(), hostptr, CL_TRUE);
	}
};

template <class T> template <class Expr>
std::map<cl_context, bool> vector<T>::exdata<Expr>::compiled;

template <class T> template <class Expr>
std::map<cl_context, cl::Kernel> vector<T>::exdata<Expr>::kernel;

template <class T> template <class Expr>
std::map<cl_context, size_t> vector<T>::exdata<Expr>::wgsize;

/// Copy device vector to host vector.
template <class T>
void copy(const vex::vector<T> &dv, std::vector<T> &hv, cl_bool blocking = CL_TRUE) {
    dv.read_data(0, dv.size(), hv.data(), blocking);
}

/// Copy host vector to device vector.
template <class T>
void copy(const std::vector<T> &hv, vex::vector<T> &dv, cl_bool blocking = CL_TRUE) {
    dv.write_data(0, dv.size(), hv.data(), blocking);
}

template<class Iterator, class Enable = void>
struct stored_on_device {
    static const bool value = false;
};

template<class Iterator>
struct stored_on_device<Iterator, typename std::enable_if<Iterator::device_iterator>::type> {
    static const bool value = true;
};

/// Copy range from device vector to host vector.
template<class InputIterator, class OutputIterator>
typename std::enable_if<
    std::is_same<
	typename std::iterator_traits<InputIterator>::value_type,
	typename std::iterator_traits<OutputIterator>::value_type
	>::value &&
    stored_on_device<InputIterator>::value &&
    !stored_on_device<OutputIterator>::value,
    OutputIterator
    >::type
copy(InputIterator first, InputIterator last,
	OutputIterator result, cl_bool blocking = CL_TRUE)
{
    first.vec.read_data(first.pos, last - first, &result[0], blocking);
    return result + (last - first);
}

/// Copy range from host vector to device vector.
template<class InputIterator, class OutputIterator>
typename std::enable_if<
    std::is_same<
	typename std::iterator_traits<InputIterator>::value_type,
	typename std::iterator_traits<OutputIterator>::value_type
	>::value &&
    !stored_on_device<InputIterator>::value &&
    stored_on_device<OutputIterator>::value,
    OutputIterator
    >::type
copy(InputIterator first, InputIterator last,
	OutputIterator result, cl_bool blocking = CL_TRUE)
{
    result.vec.write_data(result.pos, last - first, &first[0], blocking);
    return result + (last - first);
}

/// Swap two vectors.
template <typename T>
void swap(vector<T> &x, vector<T> &y) {
    x.swap(y);
}

/// Expression template.
template <class LHS, char OP, class RHS>
struct BinaryExpression : public expression {
    BinaryExpression(const LHS &lhs, const RHS &rhs) : lhs(lhs), rhs(rhs) {}

    void preamble(std::ostream &os, std::string name) const {
	lhs.preamble(os, name + "l");
	rhs.preamble(os, name + "r");
    }

    std::string kernel_name() const {
	// Polish notation.
	switch (OP) {
	    case '+':
		return "p" + lhs.kernel_name() + rhs.kernel_name();
	    case '-':
		return "m" + lhs.kernel_name() + rhs.kernel_name();
	    case '*':
		return "t" + lhs.kernel_name() + rhs.kernel_name();
	    case '/':
		return "d" + lhs.kernel_name() + rhs.kernel_name();
	    default:
		throw "unknown operation";
	}
    }

    void kernel_prm(std::ostream &os, std::string name = "") const {
	lhs.kernel_prm(os, name + "l");
	rhs.kernel_prm(os, name + "r");
    }

    void kernel_expr(std::ostream &os, std::string name = "") const {
	os << "(";
	lhs.kernel_expr(os, name + "l");
	os << " " << OP << " ";
	rhs.kernel_expr(os, name + "r");
	os << ")";
    }

    void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
	lhs.kernel_args(k, devnum, pos);
	rhs.kernel_args(k, devnum, pos);
    }

    size_t part_size(uint dev) const {
	return std::max(lhs.part_size(dev), rhs.part_size(dev));
    }

    const KernelGenerator<LHS> lhs;
    const KernelGenerator<RHS> rhs;
};

/// Sum of two expressions.
template <class LHS, class RHS>
typename std::enable_if<
    valid_expression<LHS>::value &&
    valid_expression<RHS>::value,
    BinaryExpression<LHS, '+', RHS>
    >::type
    operator+(const LHS &lhs, const RHS &rhs) {
	return BinaryExpression<LHS,'+',RHS>(lhs, rhs);
    }

/// Difference of two expressions.
template <class LHS, class RHS>
typename std::enable_if<
    valid_expression<LHS>::value &&
    valid_expression<RHS>::value,
    BinaryExpression<LHS, '-', RHS>
    >::type
    operator-(const LHS &lhs, const RHS &rhs) {
	return BinaryExpression<LHS,'-',RHS>(lhs, rhs);
    }

/// Product of two expressions.
template <class LHS, class RHS>
typename std::enable_if<
    valid_expression<LHS>::value &&
    valid_expression<RHS>::value,
    BinaryExpression<LHS, '*', RHS>
    >::type
    operator*(const LHS &lhs, const RHS &rhs) {
	return BinaryExpression<LHS,'*',RHS>(lhs, rhs);
    }

/// Division of two expressions.
template <class LHS, class RHS>
typename std::enable_if<
    valid_expression<LHS>::value &&
    valid_expression<RHS>::value,
    BinaryExpression<LHS, '/', RHS>
    >::type
    operator/(const LHS &lhs, const RHS &rhs) {
	return BinaryExpression<LHS,'/',RHS>(lhs, rhs);
    }

/// \internal Custom user function expression template
template<class RetType, class... ArgType>
struct UserFunctionFamily {
    template <const char *body, class... Expr>
    class Function : public expression {
	public:
	    Function(const Expr&... expr) : expr(expr...) {}

	    void preamble(std::ostream &os, std::string name) const {
		build_preamble<0>(os, name);

		os << type_name<RetType>() << " " << name << "_fun(";

		build_arg_list<ArgType...>(os, 0);

		os << "\n\t)\n{\n" << body << "\n}\n";
	    }

	    std::string kernel_name() const {
		return std::string("uf") + build_kernel_name<0>();
	    }

	    void kernel_prm(std::ostream &os, std::string name) const {
		build_kernel_prm<0>(os, name);
	    }

	    void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
		set_kernel_args<0>(k, devnum, pos);
	    }

	    void kernel_expr(std::ostream &os, std::string name) const {
		os << name << "_fun(";
		build_kernel_expr<0>(os, name);
		os << ")";
	    }

	    size_t part_size(uint dev) const {
		return get_part_size<0>(dev);
	    }
	private:
	    std::tuple<const Expr&...> expr;

	    //------------------------------------------------------------
	    template <int num>
	    typename std::enable_if<num == sizeof...(ArgType), std::string>::type
	    build_kernel_name() const {
		return "";
	    }

	    template <int num>
	    typename std::enable_if<num < sizeof...(ArgType), std::string>::type
	    build_kernel_name() const {
		return std::get<num>(expr).kernel_name() + build_kernel_name<num + 1>();
	    }

	    //------------------------------------------------------------
	    template <int num>
	    typename std::enable_if<num == sizeof...(ArgType), void>::type
	    build_kernel_prm(std::ostream &os, std::string name) const {}

	    template <int num>
	    typename std::enable_if<num < sizeof...(ArgType), void>::type
	    build_kernel_prm(std::ostream &os, std::string name) const {
		std::ostringstream cname;
		cname << name << num + 1;
		std::get<num>(expr).kernel_prm(os, cname.str());
		build_kernel_prm<num + 1>(os, name);
	    }

	    //------------------------------------------------------------
	    template <int num>
	    typename std::enable_if<num == sizeof...(ArgType), void>::type
	    set_kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {}

	    template <int num>
	    typename std::enable_if<num < sizeof...(ArgType), void>::type
	    set_kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
		std::get<num>(expr).kernel_args(k, devnum, pos);
		set_kernel_args<num + 1>(k, devnum, pos);
	    }

	    //------------------------------------------------------------
	    template <int num>
	    typename std::enable_if<num == sizeof...(ArgType), void>::type
	    build_kernel_expr(std::ostream &os, std::string name) const {}

	    template <int num>
	    typename std::enable_if<num < sizeof...(ArgType), void>::type
	    build_kernel_expr(std::ostream &os, std::string name) const {
		std::ostringstream cname;
		cname << name << num + 1;
		std::get<num>(expr).kernel_expr(os, cname.str());
		if (num + 1 < sizeof...(ArgType)) {
		    os << ", ";
		    build_kernel_expr<num + 1>(os, name);
		}
	    }

	    //------------------------------------------------------------
	    template <int num>
	    typename std::enable_if<num == sizeof...(ArgType), void>::type
	    build_preamble(std::ostream &os, std::string name) const {}

	    template <int num>
	    typename std::enable_if<num < sizeof...(ArgType), void>::type
	    build_preamble(std::ostream &os, std::string name) const {
		std::ostringstream cname;
		cname << name << num + 1;
		std::get<num>(expr).preamble(os, cname.str());
		build_preamble<num + 1>(os, name);
	    }

	    //------------------------------------------------------------
	    template <class T>
	    void build_arg_list(std::ostream &os, uint num) const {
		os << "\n\t" << type_name<T>() << " prm" << num + 1;
	    }

	    template <class T, class... Args>
	    typename std::enable_if<sizeof...(Args), void>::type
	    build_arg_list(std::ostream &os, uint num) const {
		os << "\n\t" << type_name<T>() << " prm" << num + 1 << ",";
		build_arg_list<Args...>(os, num + 1);
	    }

	    //------------------------------------------------------------
	    template <int num>
	    typename std::enable_if<num == sizeof...(ArgType), size_t>::type
	    get_part_size(uint dev) const {
		return 0;
	    }

	    template <int num>
	    typename std::enable_if<num < sizeof...(ArgType), size_t>::type
	    get_part_size(uint dev) const {
		return std::max(
			std::get<num>(expr).part_size(dev),
			get_part_size<num + 1>(dev)
			);
	    }

    };
};

/// Custom user function
/**
 * Is used for introduction of custom functions into expressions. For example,
 * to count how many elements in x vector are greater than their counterparts
 * in y vector, the following code may be used:
 * \code
 * // Body of the function. Has to be extern const char[] in order to be used
 * // as template parameter.
 * extern const char one_greater_than_other[] = "return prm1 > prm2 ? 1 : 0;";
 *
 * size_t count_if_greater(const vex::vector<float> &x, const vex::vector<float> &y) {
 *     Reductor<size_t, SUM> sum(x.queue_list());
 *
 *     UserFunction<one_greater_than_other, size_t, float, float> greater;
 *
 *     return sum(greater(x, y));
 * }
 * \endcode
 * \param body Body of user function. Parameters are named prm1, ..., prm<n>.
 * \param RetType return type of the function.
 * \param ArgType types of function arguments.
 */
template<const char *body, class RetType, class... ArgType>
struct UserFunction {
    /// Apply user function to the list of expressions.
    /**
     * Number of expressions in the list has to coincide with number of
     * ArgTypes
     */
    template <class... Expr>
    typename std::enable_if<sizeof...(ArgType) == sizeof...(Expr),
    typename UserFunctionFamily<RetType, ArgType...>::template Function<body, Expr...>
    >::type
    operator()(const Expr&... expr) const {
	return typename UserFunctionFamily<RetType, ArgType...>::template Function<body, Expr...>(expr...);
    }
};

/// \internal Unary expression template.
template <const char *func_name, class Expr>
struct UnaryExpression : public expression {
    UnaryExpression(const Expr &expr) : expr(expr) {}

    void preamble(std::ostream &os, std::string name) const {
	expr.preamble(os, name);
    }

    std::string kernel_name() const {
	return func_name + expr.kernel_name();
    }

    void kernel_expr(std::ostream &os, std::string name) const {
	os << func_name << "(";
	expr.kernel_expr(os, name);
	os << ")";
    }

    void kernel_prm(std::ostream &os, std::string name) const {
	expr.kernel_prm(os, name);
    }

    void kernel_args(cl::Kernel &k, uint devnum, uint &pos) const {
	expr.kernel_args(k, devnum, pos);
    }

    size_t part_size(uint dev) const {
	return expr.part_size(dev);
    }

    private:
	const Expr &expr;
};

extern const char acos_fun[]   = "acos";
extern const char acosh_fun[]  = "acosh";
extern const char acospi_fun[] = "acospi";
extern const char asin_fun[]   = "asin";
extern const char asinh_fun[]  = "asinh";
extern const char asinpi_fun[] = "asinpi";
extern const char atan_fun[]   = "atan";
extern const char atanh_fun[]  = "atanh";
extern const char atanpi_fun[] = "atanpi";
extern const char cbrt_fun[]   = "cbrt";
extern const char ceil_fun[]   = "ceil";
extern const char cos_fun[]    = "cos";
extern const char cosh_fun[]   = "cosh";
extern const char cospi_fun[]  = "cospi";
extern const char erfc_fun[]   = "erfc";
extern const char erf_fun[]    = "erf";
extern const char exp_fun[]    = "exp";
extern const char exp2_fun[]   = "exp2";
extern const char exp10_fun[]  = "exp10";
extern const char expm1_fun[]  = "expm1";
extern const char fabs_fun[]   = "fabs";
extern const char floor_fun[]  = "floor";
extern const char ilogb_fun[]  = "ilogb";
extern const char lgamma_fun[] = "lgamma";
extern const char log_fun[]    = "log";
extern const char log2_fun[]   = "log2";
extern const char log10_fun[]  = "log10";
extern const char log1p_fun[]  = "log1p";
extern const char logb_fun[]   = "logb";
extern const char nan_fun[]    = "nan";
extern const char rint_fun[]   = "rint";
extern const char rootn_fun[]  = "rootn";
extern const char round_fun[]  = "round";
extern const char rsqrt_fun[]  = "rsqrt";
extern const char sin_fun[]    = "sin";
extern const char sinh_fun[]   = "sinh";
extern const char sinpi_fun[]  = "sinpi";
extern const char sqrt_fun[]   = "sqrt";
extern const char tan_fun[]    = "tan";
extern const char tanh_fun[]   = "tanh";
extern const char tanpi_fun[]  = "tanpi";
extern const char tgamma_fun[] = "tgamma";
extern const char trunc_fun[]  = "trunc";

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<acos_fun, Expr>>::type
acos(const Expr &expr) {
return UnaryExpression<acos_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<acosh_fun, Expr>>::type
acosh(const Expr &expr) {
return UnaryExpression<acosh_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<acospi_fun, Expr>>::type
acospi(const Expr &expr) {
return UnaryExpression<acospi_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<asin_fun, Expr>>::type
asin(const Expr &expr) {
return UnaryExpression<asin_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<asinh_fun, Expr>>::type
asinh(const Expr &expr) {
return UnaryExpression<asinh_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<asinpi_fun, Expr>>::type
asinpi(const Expr &expr) {
return UnaryExpression<asinpi_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<atan_fun, Expr>>::type
atan(const Expr &expr) {
return UnaryExpression<atan_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<atanh_fun, Expr>>::type
atanh(const Expr &expr) {
return UnaryExpression<atanh_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<atanpi_fun, Expr>>::type
atanpi(const Expr &expr) {
return UnaryExpression<atanpi_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<cbrt_fun, Expr>>::type
cbrt(const Expr &expr) {
return UnaryExpression<cbrt_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<ceil_fun, Expr>>::type
ceil(const Expr &expr) {
return UnaryExpression<ceil_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<cos_fun, Expr>>::type
cos(const Expr &expr) {
return UnaryExpression<cos_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<cosh_fun, Expr>>::type
cosh(const Expr &expr) {
return UnaryExpression<cosh_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<cospi_fun, Expr>>::type
cospi(const Expr &expr) {
return UnaryExpression<cospi_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<erfc_fun, Expr>>::type
erfc(const Expr &expr) {
return UnaryExpression<erfc_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<erf_fun, Expr>>::type
erf(const Expr &expr) {
return UnaryExpression<erf_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<exp_fun, Expr>>::type
exp(const Expr &expr) {
return UnaryExpression<exp_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<exp2_fun, Expr>>::type
exp2(const Expr &expr) {
return UnaryExpression<exp2_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<exp10_fun, Expr>>::type
exp10(const Expr &expr) {
return UnaryExpression<exp10_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<expm1_fun, Expr>>::type
expm1(const Expr &expr) {
return UnaryExpression<expm1_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<fabs_fun, Expr>>::type
fabs(const Expr &expr) {
return UnaryExpression<fabs_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<floor_fun, Expr>>::type
floor(const Expr &expr) {
return UnaryExpression<floor_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<ilogb_fun, Expr>>::type
ilogb(const Expr &expr) {
return UnaryExpression<ilogb_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<lgamma_fun, Expr>>::type
lgamma(const Expr &expr) {
return UnaryExpression<lgamma_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<log_fun, Expr>>::type
log(const Expr &expr) {
return UnaryExpression<log_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<log2_fun, Expr>>::type
log2(const Expr &expr) {
return UnaryExpression<log2_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<log10_fun, Expr>>::type
log10(const Expr &expr) {
return UnaryExpression<log10_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<log1p_fun, Expr>>::type
log1p(const Expr &expr) {
return UnaryExpression<log1p_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<logb_fun, Expr>>::type
logb(const Expr &expr) {
return UnaryExpression<logb_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<nan_fun, Expr>>::type
nan(const Expr &expr) {
return UnaryExpression<nan_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<rint_fun, Expr>>::type
rint(const Expr &expr) {
return UnaryExpression<rint_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<rootn_fun, Expr>>::type
rootn(const Expr &expr) {
return UnaryExpression<rootn_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<round_fun, Expr>>::type
round(const Expr &expr) {
return UnaryExpression<round_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<rsqrt_fun, Expr>>::type
rsqrt(const Expr &expr) {
return UnaryExpression<rsqrt_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<sin_fun, Expr>>::type
sin(const Expr &expr) {
return UnaryExpression<sin_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<sinh_fun, Expr>>::type
sinh(const Expr &expr) {
return UnaryExpression<sinh_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<sinpi_fun, Expr>>::type
sinpi(const Expr &expr) {
return UnaryExpression<sinpi_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<sqrt_fun, Expr>>::type
sqrt(const Expr &expr) {
return UnaryExpression<sqrt_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<tan_fun, Expr>>::type
tan(const Expr &expr) {
return UnaryExpression<tan_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<tanh_fun, Expr>>::type
tanh(const Expr &expr) {
return UnaryExpression<tanh_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<tanpi_fun, Expr>>::type
tanpi(const Expr &expr) {
return UnaryExpression<tanpi_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<tgamma_fun, Expr>>::type
tgamma(const Expr &expr) {
return UnaryExpression<tgamma_fun, Expr>(expr);
}

template <class Expr>
typename std::enable_if<Expr::is_expression,
UnaryExpression<trunc_fun, Expr>>::type
trunc(const Expr &expr) {
return UnaryExpression<trunc_fun, Expr>(expr);
}


/// Returns device weight after simple bandwidth test
double device_vector_perf(
	const cl::Context &context, const cl::Device &device,
	size_t test_size = 1024U * 1024U
	)
{
    static std::map<cl_device_id, double> dev_weights;

    auto dw = dev_weights.find(device());

    if (dw == dev_weights.end()) {
	std::vector<cl::CommandQueue> queue(1,
		cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE)
		);

	// Allocate test vectors on current device and measure execution
	// time of a simple kernel.
	vex::vector<float> a(queue, test_size);
	vex::vector<float> b(queue, test_size);
	vex::vector<float> c(queue, test_size);

	b = 1;
	c = 2;

	// Skip the first run.
	a = b + c;

	// Measure the second run.
	profiler prof(queue);
	prof.tic_cl("");
	a = b + c;
	return dev_weights[device()] = 1 / prof.toc("");
    } else {
	return dw->second;
    }
}

/// Partitions vector wrt to vector performance of devices.
/**
 * Launches the following kernel on each device:
 * \code
 * a = b + c;
 * \endcode
 * where a, b and c are device vectors. Each device gets portion of the vector
 * proportional to the performance of this operation.
 */
std::vector<size_t> partition_by_vector_perf(
	size_t n, const std::vector<cl::CommandQueue> &queue)
{

    std::vector<size_t> part(queue.size() + 1, 0);

    if (queue.size() > 1) {
	std::vector<double> cumsum;
	cumsum.reserve(queue.size() + 1);
	cumsum.push_back(0);

	for(auto q = queue.begin(); q != queue.end(); q++)
	    cumsum.push_back(cumsum.back() + device_vector_perf(
			q->getInfo<CL_QUEUE_CONTEXT>(),
			q->getInfo<CL_QUEUE_DEVICE>()
			));

	for(uint d = 0; d < queue.size(); d++)
	    part[d + 1] = std::min(n, alignup(n * cumsum[d + 1] / cumsum.back()));
    }

    part.back() = n;

    return part;
}

} // namespace vex

#endif