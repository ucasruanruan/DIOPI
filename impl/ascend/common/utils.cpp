/**
 * @file
 * @author DeepLink
 * @copyright  (c) 2023, DeepLink.
 */

#include "utils.hpp"

#include <functional>
#include <numeric>
#include <string>
#include <type_traits>
#include <typeinfo>

#include "../ascend_tensor.hpp"
#include "acloprunner.hpp"
namespace impl {
namespace ascend {

const char* diopiDtypeToStr(diopiDtype_t dtype) {
    switch (dtype) {
        case diopi_dtype_int8:
            return "diopi_dtype_int8";
        case diopi_dtype_uint8:
            return "diopi_dtype_uint8";
        case diopi_dtype_int16:
            return "diopi_dtype_int16";
        case diopi_dtype_uint16:
            return "diopi_dtype_uint16";
        case diopi_dtype_int32:
            return "diopi_dtype_int32";
        case diopi_dtype_uint32:
            return "diopi_dtype_uint32";
        case diopi_dtype_int64:
            return "diopi_dtype_int64";
        case diopi_dtype_uint64:
            return "diopi_dtype_uint64";
        case diopi_dtype_float16:
            return "diopi_dtype_float16";
        case diopi_dtype_float32:
            return "diopi_dtype_float32";
        case diopi_dtype_float64:
            return "diopi_dtype_float64";
        case diopi_dtype_bool:
            return "diopi_dtype_bool";
        case diopi_dtype_bfloat16:
            return "diopi_dtype_bfloat16";
        case diopi_dtype_tfloat32:
            return "diopi_dtype_tfloat32";
        case diopi_dtype_complex32:
            return "diopi_dtype_complex32";
        case diopi_dtype_complex64:
            return "diopi_dtype_complex64";
        case diopi_dtype_complex128:
            return "diopi_dtype_complex128";
        default:
            return "unsupport dtype";
    }
    return "";
}

// ascend tensor utils
diopiError_t makeTensor(diopiContextHandle_t ctx, AscendTensor& dst, const diopiSize_t* size, const diopiSize_t* stride, diopiDtype_t dtype,
                        diopiDevice_t device) {
    diopiTensorHandle_t dstPtr;
    diopiRequireTensor(ctx, &dstPtr, size, stride, dtype, device);
    dst = AscendTensor(dstPtr);
    ASCEND_CHECK_ABORT(dst.defined(), "generate Ascend Tensor failed, it's nullptr.");
    return diopiSuccess;
}

diopiError_t makeTensor(diopiContextHandle_t ctx, AscendTensor& dst, const diopiSize_t* size, diopiDtype_t dtype, diopiDevice_t device) {
    return makeTensor(ctx, dst, size, nullptr, dtype, device);
}

diopiError_t makeTensor(diopiContextHandle_t ctx, AscendTensor& dst, const std::vector<int64_t>& shape, const std::vector<int64_t>& stride, diopiDtype_t dtype,
                        diopiDevice_t device) {
    diopiSize_t shapeTmp{shape.data(), static_cast<int64_t>(shape.size())};
    if (stride.empty()) {
        return makeTensor(ctx, dst, &shapeTmp, nullptr, dtype, device);
    } else {
        diopiSize_t strideTmp{stride.data(), static_cast<int64_t>(stride.size())};
        return makeTensor(ctx, dst, &shapeTmp, &strideTmp, dtype, device);
    }
    return diopiSuccess;
}

diopiError_t makeTensorLike(diopiContextHandle_t ctx, AscendTensor& dst, const AscendTensor& src, diopiDtype_t dtype) {
    if (diopi_dtype_unsupported == dtype) {
        return makeTensor(ctx, dst, src.shape(), src.stride(), src.dtype(), src.device());
    } else {
        return makeTensor(ctx, dst, src.shape(), src.stride(), dtype, src.device());
    }
}

diopiError_t makeTensor(diopiContextHandle_t ctx, AscendTensor& dst, const std::vector<int64_t>& shape, diopiDtype_t dtype) {
    return makeTensor(ctx, dst, shape, std::vector<int64_t>{}, dtype, diopi_device);
}

diopiError_t makeTensorFromScalar(diopiContextHandle_t ctx, AscendTensor& dst, const diopiScalar_t* scalar, diopiDevice_t device) {
    std::vector<int64_t> shape{1};
    makeTensor(ctx, dst, shape, scalar->stype);
    auto th = const_cast<diopiTensorHandle_t>(static_cast<diopiConstTensorHandle_t>(dst));
    if (diopi_device == device) {
        return diopiFill(ctx, th, scalar);
    } else {
        void* ptr;
        diopiGetTensorData(th, &ptr);
        if (isFloatingType(scalar->stype)) {
            reinterpret_cast<double*>(ptr)[0] = getValue<double>(scalar);
        } else {
            reinterpret_cast<int64_t*>(ptr)[0] = getValue<int64_t>(scalar);
        }
        return diopiSuccess;
    }
}

diopiError_t fillNan(diopiContextHandle_t ctx, AscendTensor& src) {
    // get nan value tensor
    diopiTensorHandle_t nanValue;
    auto zeroValueScalar = constructDiopiScalarT(diopi_dtype_float64, 0.0);
    makeTensorFromScalar(ctx, &zeroValueScalar, &nanValue, diopi_dtype_float32, diopi_device);
    diopiDivInpScalar(ctx, nanValue, &zeroValueScalar, diopiRoundMode_t::RoundModeNone);

    diopiTensorHandle_t onePtr;
    makeOnesLike(ctx, &onePtr, src.tensorHandle());
    AscendTensor nan(nanValue), one(onePtr);
    castTensor(ctx, one, diopi_dtype_bool);
    diopiMaskedFillInp(ctx, const_cast<diopiTensorHandle_t>(src.tensorHandle()), one.tensorHandle(), nan.tensorHandle());
    return diopiSuccess;
}

diopiError_t reshape(diopiContextHandle_t ctx, const AscendTensor& src, AscendTensor& dst, const std::vector<int64_t>& shape) {
    ASCEND_CHECK_ABORT(src.isContiguous(), "now only contiguous tensor support reshape by shape.");
    if (src.isSame(dst)) {
        dst.view(shape);
        return diopiSuccess;
    }

    // make dst tensor with `shape`
    AscendTensor tmp = src;
    tmp.view(shape);
    makeTensorLike(ctx, dst, tmp);

    auto sourcePtr = const_cast<void*>(src.data());
    auto destPtr = const_cast<void*>(dst.data());
    diopiStreamHandle_t stream;
    diopiGetStream(ctx, &stream);
    aclrtMemcpyAsync(destPtr, dst.getAclMemBufferSize(), sourcePtr, src.getAclMemBufferSize(), ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
    aclrtSynchronizeStream(stream);

    return diopiSuccess;
}

diopiError_t aclAsStridedCore(diopiContextHandle_t ctx, const AscendTensor& src, AscendTensor& dst) {
    diopiTensorHandle_t targetObj = const_cast<diopiTensorHandle_t>(static_cast<diopiConstTensorHandle_t>(dst));
    AclOpRunner<4, 1>("AsStrided", ctx)
        .addInput(src.data(), src.getAclMemBufferSize(), src.getAclMemShape(), src.getAclDataFormat(), src.dtype())
        .addConstInput(src.shape())
        .addConstInput(src.stride())
        .addConstInput(0, diopi_dtype_int64)
        .addOutput(targetObj)
        .run();

    // update Ascend Tensor attribute.
    dst = AscendTensor(targetObj);
    return diopiSuccess;
}

diopiError_t contiguous(diopiContextHandle_t ctx, const AscendTensor& src, AscendTensor& dst, diopiMemoryFormat_t format) {
    if (src.isContiguous(format)) {
        dst = const_cast<AscendTensor&>(src);
        return diopiSuccess;
    }

    return aclAsStrided(ctx, src, dst);
}

diopiError_t castTensor(diopiContextHandle_t ctx, const AscendTensor& src, AscendTensor& dst) {
    ASCEND_CHECK_ABORT(dst.shape() == src.shape(), "required input and output has the same shape.");
    if (src.data() == dst.data()) {
        return diopiSuccess;
    }

    auto dstPtr = const_cast<diopiTensorHandle_t>(static_cast<diopiConstTensorHandle_t>(dst));
    diopiCastDtype(ctx, dstPtr, static_cast<diopiConstTensorHandle_t>(src));
    dst = AscendTensor(dstPtr);

    return diopiSuccess;
}

diopiError_t castTensor(diopiContextHandle_t ctx, const std::vector<AscendTensor>& src, std::vector<AscendTensor>& dst, diopiDtype_t supportDtype) {
    ASCEND_CHECK_ABORT(src.size() == dst.size(), "require input size equal output size.");
    for (int i = 0; i < src.size(); ++i) {
        CHECK_ASCENDRT(castTensor(ctx, src[i], dst[i]));
    }
    return diopiSuccess;
}

diopiError_t castTensor(diopiContextHandle_t ctx, AscendTensor& src, diopiDtype_t dtype) {
    AscendTensor temp;
    makeTensorLike(ctx, temp, src, dtype);
    castTensor(ctx, src, temp);
    src = temp;
    return diopiSuccess;
}

diopiError_t aclAsStrided(diopiContextHandle_t ctx, const AscendTensor& src, AscendTensor& dst) {
    if (src.dtype() != diopi_dtype_float64) {
        return aclAsStridedCore(ctx, src, dst);
    } else {
        AscendTensor srcCpy = const_cast<AscendTensor&>(src);
        castTensor(ctx, srcCpy, diopi_dtype_float32);
        castTensor(ctx, dst, diopi_dtype_float32);

        return aclAsStridedCore(ctx, srcCpy, dst);
    }
}

// diopi tensor utils
diopiError_t fillTensor(diopiContextHandle_t ctx, diopiTensorHandle_t out, float val) {
    auto valScalar = constructDiopiScalarT(diopi_dtype_float64, val);
    diopiFill(ctx, out, &valScalar);
    return diopiSuccess;
}

diopiError_t fillTensor(diopiContextHandle_t ctx, diopiTensorHandle_t out, int val) {
    auto valScalar = constructDiopiScalarT(diopi_dtype_int64, val);
    diopiFill(ctx, out, &valScalar);
    return diopiSuccess;
}

diopiError_t fillTensor(diopiContextHandle_t ctx, diopiTensorHandle_t out, double val) {
    auto valScalar = constructDiopiScalarT(diopi_dtype_float64, val);
    diopiFill(ctx, out, &valScalar);
    return diopiSuccess;
}

diopiTensorHandle_t createTensorIfNullptrOrConstCast(diopiContextHandle_t ctx, diopiConstTensorHandle_t in, diopiSize_t& shape, diopiDtype_t dtype,
                                                     bool isFillingRequired, double value) {
    diopiTensorHandle_t out;
    if (nullptr == in) {
        diopiRequireTensor(ctx, &out, &shape, nullptr, dtype, diopi_device);
        if (isFillingRequired) {
            fillTensor(ctx, out, value);
        }
    } else {
        out = const_cast<diopiTensorHandle_t>(in);
    }
    return out;
}

diopiError_t makeTensorFromScalar(diopiContextHandle_t ctx, const diopiScalar_t* scalar, diopiTensorHandle_t* out, diopiDtype_t dtype, diopiDevice_t device) {
    int64_t sizeTmp[1] = {1};
    diopiSize_t sSize = arrayToDiopiSize(sizeTmp, 1);
    diopiTensorHandle_t outCopy;
    if (device == diopi_host) {
        diopiRequireTensor(ctx, &outCopy, &sSize, nullptr, dtype, diopi_host);
        void* ptr;
        diopiGetTensorData(outCopy, &ptr);
        switch (dtype) {
            case diopiDtype_t::diopi_dtype_float32:
                *reinterpret_cast<float*>(ptr) = getValue<float>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_float64:
                *reinterpret_cast<double*>(ptr) = getValue<double>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_int32:
                *reinterpret_cast<int*>(ptr) = getValue<int>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_int64:
                *reinterpret_cast<int64_t*>(ptr) = getValue<int64_t>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_uint8:
                *reinterpret_cast<uint8_t*>(ptr) = getValue<uint8_t>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_int8:
                *reinterpret_cast<int8_t*>(ptr) = getValue<int8_t>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_bool:
                *reinterpret_cast<bool*>(ptr) = getValue<bool>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_int16:
                *reinterpret_cast<int16_t*>(ptr) = getValue<int16_t>(scalar);
                break;
            case diopiDtype_t::diopi_dtype_uint16:
                *reinterpret_cast<uint16_t*>(ptr) = getValue<uint16_t>(scalar);
                break;
            default:
                error("dtype %d not supported on host", dtype);
        }
        *out = outCopy;
    } else if (device == diopi_device) {
        diopiTensorHandle_t outCopyDev;
        void *src, *dst;
        if (isFloatingType(dtype)) {
            diopiRequireTensor(ctx, &outCopy, &sSize, nullptr, diopi_dtype_float64, diopi_host);
            diopiRequireTensor(ctx, &outCopyDev, &sSize, nullptr, diopi_dtype_float64, diopi_device);
            diopiGetTensorData(outCopy, &src);
            reinterpret_cast<double*>(src)[0] = getValue<double>(scalar);
        } else if (isIntegralTypeWithBool(dtype)) {
            diopiRequireTensor(ctx, &outCopy, &sSize, nullptr, diopi_dtype_int64, diopi_host);
            diopiRequireTensor(ctx, &outCopyDev, &sSize, nullptr, diopi_dtype_int64, diopi_device);
            diopiGetTensorData(outCopy, &src);
            reinterpret_cast<int64_t*>(src)[0] = getValue<int64_t>(scalar);
        } else {
            error("dtype %d not supported on device", dtype);
        }
        int64_t elemsize;
        diopiStreamHandle_t stream;
        diopiGetTensorElemSize(outCopy, &elemsize);
        diopiGetStream(ctx, &stream);
        diopiRequireTensor(ctx, out, &sSize, nullptr, dtype, diopi_device);
        diopiGetTensorData(outCopyDev, &dst);
        CALL_ACLRT(aclrtMemcpyAsync(dst, elemsize, src, elemsize, ACL_MEMCPY_HOST_TO_DEVICE, stream));
        CALL_ACLRT(aclrtSynchronizeStream(stream));
        diopiCastDtype(ctx, *out, outCopyDev);
    } else {
        error("device %d not supported", device);
    }
    return diopiSuccess;
}

diopiError_t makeTensorFromScalar(diopiContextHandle_t ctx, const diopiScalar_t* scalar, diopiTensorHandle_t* out, diopiDevice_t device) {
    return makeTensorFromScalar(ctx, scalar, out, scalar->stype, device);
}

diopiError_t makeTensorFromSize(diopiContextHandle_t ctx, const diopiSize_t* size, diopiTensorHandle_t* out, diopiDtype_t dtype) {
    int64_t len = size->len;
    int64_t sizeTmp[1] = {len};
    diopiSize_t sSize = arrayToDiopiSize(sizeTmp, 1);
    diopiRequireTensor(ctx, out, &sSize, nullptr, dtype, diopi_host);
    if (len > 0) {
        void* dst = nullptr;
        diopiGetTensorData(*out, &dst);
        if (dtype == diopi_dtype_int64) {
            for (int i = 0; i < len; i++) {
                reinterpret_cast<int64_t*>(dst)[i] = (int64_t)size->data[i];
            }
        } else if (dtype == diopi_dtype_int32) {
            for (int i = 0; i < len; i++) {
                reinterpret_cast<int32_t*>(dst)[i] = (int32_t)size->data[i];
            }
        } else if (dtype == diopi_dtype_int16) {
            for (int i = 0; i < len; i++) {
                reinterpret_cast<int16_t*>(dst)[i] = (int16_t)size->data[i];
            }
        } else if (dtype == diopi_dtype_bool) {
            for (int i = 0; i < len; i++) {
                reinterpret_cast<bool*>(dst)[i] = static_cast<bool>(size->data[i]);
            }
        }
    }
    return diopiSuccess;
}

diopiError_t makeTensorFromSize(diopiContextHandle_t ctx, const diopiSize_t* size, diopiTensorHandle_t* out) {
    return makeTensorFromSize(ctx, size, out, diopi_dtype_int64);
}

diopiError_t makeTensorLike(diopiContextHandle_t ctx, diopiTensorHandle_t* out, diopiConstTensorHandle_t src, diopiDtype_t dtype) {
    diopiDevice_t device;
    diopiSize_t size, stride;
    diopiGetTensorDevice(src, &device);
    diopiGetTensorShape(src, &size);
    diopiGetTensorStride(src, &stride);
    diopiRequireTensor(ctx, out, &size, &stride, dtype, device);
    return diopiSuccess;
}

diopiError_t makeTensorLike(diopiContextHandle_t ctx, diopiTensorHandle_t* out, diopiConstTensorHandle_t src) {
    diopiDtype_t dtype;
    diopiGetTensorDtype(src, &dtype);
    return makeTensorLike(ctx, out, src, dtype);
}

diopiError_t makeOnesLike(diopiContextHandle_t ctx, diopiTensorHandle_t* out, diopiConstTensorHandle_t src, diopiDtype_t dtype) {
    makeTensorLike(ctx, out, src, dtype);
    fillTensor(ctx, *out, static_cast<float>(1.0));
    return diopiSuccess;
}

diopiError_t makeOnesLike(diopiContextHandle_t ctx, diopiTensorHandle_t* out, diopiConstTensorHandle_t src) {
    diopiDtype_t dtype;
    diopiGetTensorDtype(src, &dtype);
    return makeOnesLike(ctx, out, src, dtype);
}

diopiError_t negativeInputRtnFillNan(diopiContextHandle_t ctx, diopiTensorHandle_t out, diopiConstTensorHandle_t input) {
    // get nan value tensor
    diopiTensorHandle_t nanValue;
    auto nanValueScalar = diopiScalar_t();
    nanValueScalar.stype = diopi_dtype_float64;
    nanValueScalar.fval = 0.0;
    makeTensorFromScalar(ctx, &nanValueScalar, &nanValue, diopi_dtype_float32, diopi_device);
    auto zeroValueScalar = diopiScalar_t();
    zeroValueScalar.stype = diopi_dtype_float64;
    zeroValueScalar.fval = 0.0;
    diopiDivInpScalar(ctx, nanValue, &zeroValueScalar, diopiRoundMode_t::RoundModeNone);

    diopiDtype_t inputDtype;
    diopiGetTensorDtype(input, &inputDtype);
    diopiTensorHandle_t inputTemp;
    if (diopi_dtype_float16 == inputDtype) {
        makeTensorLike(ctx, &inputTemp, input, diopi_dtype_float32);
        diopiCastDtype(ctx, inputTemp, input);
    } else {
        inputTemp = const_cast<diopiTensorHandle_t>(input);
    }

    // get negative mask
    diopiTensorHandle_t mask;
    makeTensorLike(ctx, &mask, inputTemp, diopi_dtype_bool);
    diopiLtScalar(ctx, mask, inputTemp, &zeroValueScalar);

    // NaN of float16 can only be cast from NaN of float64
    diopiDtype_t outputDtype;
    diopiGetTensorDtype(out, &outputDtype);
    diopiTensorHandle_t outputTemp;
    if (diopi_dtype_float16 == outputDtype) {
        makeTensorLike(ctx, &outputTemp, out, diopi_dtype_float64);
        diopiCastDtype(ctx, outputTemp, out);
    } else {
        outputTemp = out;
    }

    // masked_fill nan
    diopiMaskedFillInp(ctx, outputTemp, mask, nanValue);

    if (diopi_dtype_float16 == outputDtype) {
        diopiCastDtype(ctx, out, outputTemp);
    }

    return diopiSuccess;
}

aclDataType getAclDataType(diopiDtype_t type) {
    switch (type) {
        case diopi_dtype_float16:
            return ACL_FLOAT16;
        case diopi_dtype_float32:
            return ACL_FLOAT;
        case diopi_dtype_float64:
            return ACL_DOUBLE;
        case diopi_dtype_int8:
            return ACL_INT8;
        case diopi_dtype_uint8:
            return ACL_UINT8;
        case diopi_dtype_int16:
            return ACL_INT16;
        case diopi_dtype_uint16:
            return ACL_UINT16;
        case diopi_dtype_int32:
            return ACL_INT32;
        case diopi_dtype_uint32:
            return ACL_UINT32;
        case diopi_dtype_int64:
            return ACL_INT64;
        case diopi_dtype_uint64:
            return ACL_UINT64;
        case diopi_dtype_bool:
            return ACL_BOOL;
        case diopi_dtype_complex64:
            return ACL_COMPLEX64;
        case diopi_dtype_complex128:
            return ACL_COMPLEX128;
        default:
            ASCEND_CHECK_ABORT(false, "acl not support dioptDtype_t:%d", type);
            return ACL_DT_UNDEFINED;
    }
}

aclDataType getAclDataType(diopiConstTensorHandle_t th) {
    diopiDtype_t type;
    diopiGetTensorDtype(th, &type);
    return getAclDataType(type);
}

bool isContiguous(diopiConstTensorHandle_t tensor, diopiMemoryFormat_t format) {
    diopiSize_t size, strideDiopi;
    diopiGetTensorShape(tensor, &size);
    diopiGetTensorStride(tensor, &strideDiopi);
    auto dim = size.len;
    auto shape = size.data;
    auto strides = strideDiopi.data;
    int64_t stride = 1;

    if (format == diopiMemoryFormat_t::Contiguous) {
        for (int64_t i = dim - 1; i >= 0; i--) {
            const auto& shapeD = shape[i];
            if (shapeD != 1) {
                if (strides[i] != stride) {
                    return false;
                }
            }
            stride *= shapeD;
        }
    } else if (format == diopiMemoryFormat_t::ChannelsLast) {
        if (dim != 4) return false;
        for (auto& i : {1, 3, 2, 0}) {
            const auto& shapeD = shape[i];
            if (shapeD != 1) {
                // shape_d != 1 help dealing with shape like [2, 2048, 1, 1]
                if (strides[i] != stride) {
                    return false;
                }
            }
            stride *= shapeD;
        }
    } else if (format == diopiMemoryFormat_t::ChannelsLast3d) {
        if (dim != 5) return false;
        for (auto& i : {1, 4, 3, 2, 0}) {
            const auto& shapeD = shape[i];
            if (shapeD != 1) {
                if (strides[i] != stride) {
                    return false;
                }
            }
            stride *= shape[i];
        }
    } else if (format == diopiMemoryFormat_t::ChannelsLast1d) {
        if (dim != 3) return false;
        for (auto& i : {1, 2, 0}) {
            const auto& shapeD = shape[i];
            if (shapeD != 1) {
                if (strides[i] != stride) {
                    return false;
                }
            }
            stride *= shape[i];
        }
    }
    return true;
}

std::vector<int64_t> getBaseShape(diopiConstTensorHandle_t src) {
    std::vector<int64_t> baseShapeVec;
    diopiSize_t shape;
    diopiGetTensorShape(src, &shape);
    if (isContiguous(src)) {
        if (shape.len > 0) {
            baseShapeVec.resize(shape.len);
            for (int64_t i = 0; i < shape.len; i++) {
                baseShapeVec[i] = shape.data[i];
            }
        } else {
            baseShapeVec.push_back(1);
        }

    } else {
        diopiSize_t stride;
        diopiGetTensorStride(src, &stride);
        int64_t maxStride = 0, maxIdx = -1;
        for (int64_t i = 0; i < stride.len; i++) {
            if (stride.data[i] > maxStride) {
                maxStride = stride.data[i];
                maxIdx = i;
            }
        }
        if (maxStride > 0) {
            baseShapeVec.push_back(shape.data[maxIdx] * maxStride);
        } else {
            baseShapeVec.push_back(1);
        }
    }
    return baseShapeVec;
}

int64_t getBaseBufferSize(diopiConstTensorHandle_t src) {
    int64_t numel = 1, elemsize;
    diopiSize_t shape;
    diopiGetTensorShape(src, &shape);
    diopiGetTensorElemSize(src, &elemsize);
    if (isContiguous(src)) {
        if (shape.len > 0) {
            diopiGetTensorNumel(src, &numel);
            return numel * elemsize;
        } else {
            return elemsize;
        }
    } else {
        diopiSize_t stride;
        diopiGetTensorStride(src, &stride);
        int64_t maxStride = 0, maxIdx = -1;
        for (int64_t i = 0; i < stride.len; i++) {
            if (stride.data[i] > maxStride) {
                maxStride = stride.data[i];
                maxIdx = i;
            }
        }
        if (maxStride > 0) {
            return shape.data[maxIdx] * maxStride * elemsize;
        } else {
            return elemsize;
        }
    }
}

diopiTensorHandle_t clone(diopiContextHandle_t ctx, diopiConstTensorHandle_t src) {
    diopiTensorHandle_t srcClone;
    diopiSize_t size, stride;
    diopiDtype_t dtype;
    diopiGetTensorDtype(src, &dtype);
    diopiGetTensorShape(src, &size);
    diopiRequireTensor(ctx, &srcClone, &size, nullptr, dtype, diopi_device);
    diopiGetTensorStride(src, &stride);
    if (isContiguous(src)) {
        diopiCopyInp(ctx, src, srcClone);
    } else {
        AscendTensor srcAt(src), srcCloneAt(srcClone);
        aclAsStrided(ctx, srcAt, srcCloneAt);
        srcClone = const_cast<diopiTensorHandle_t>(static_cast<diopiConstTensorHandle_t>(srcCloneAt));
    }
    return srcClone;
}

diopiTensorHandle_t contiguous(diopiContextHandle_t ctx, diopiConstTensorHandle_t src, diopiMemoryFormat_t format) {
    if (isContiguous(src, format)) {
        return const_cast<diopiTensorHandle_t>(src);
    } else {
        return clone(ctx, src);
    }
}

diopiTensorHandle_t contiguous(diopiContextHandle_t ctx, diopiConstTensorHandle_t src, diopiDtype_t dtype, diopiMemoryFormat_t format) {
    diopiDtype_t srcType;
    diopiGetTensorDtype(src, &srcType);
    if (srcType == dtype) {
        return contiguous(ctx, src, format);
    }
    diopiTensorHandle_t out;
    if (isContiguous(src, format)) {
        makeTensorLike(ctx, &out, src, dtype);
        diopiCastDtype(ctx, out, src);
    } else {
        diopiTensorHandle_t outTemp = contiguous(ctx, src, format);
        makeTensorLike(ctx, &out, outTemp, dtype);
        diopiCastDtype(ctx, out, outTemp);
    }
    return out;
}

diopiSize_t vectorToDiopiSize(std::vector<int64_t>& sizeVec) {
    diopiSize_t size;
    size.len = sizeVec.size();
    size.data = sizeVec.data();
    return size;
}

diopiSize_t arrayToDiopiSize(int64_t* data, int64_t len) {
    diopiSize_t size;
    size.len = len;
    size.data = data;
    return size;
}

diopiError_t transTensorTo2D(diopiContextHandle_t ctx, AscendTensor& th) {
    if (th.shape().size() < 2) return diopiErrorOccurred;
    std::vector<int64_t> dims;
    std::vector<int64_t> thShape = th.shape();
    int dim1 = std::accumulate(thShape.begin(), thShape.end() - 1, 1, std::multiplies<>());
    dims = {dim1, thShape.back()};
    th.view(dims);
    return diopiSuccess;
}

diopiError_t broadcast(diopiContextHandle_t ctx, diopiTensorHandle_t out, diopiConstTensorHandle_t input, const std::vector<int64_t>& size) {
    AscendTensor atout(out);
    const AscendTensor atinp(input);
    return broadcast(ctx, atout, atinp, size);
}

diopiError_t broadcast(diopiContextHandle_t ctx, AscendTensor& out, const AscendTensor& input, const std::vector<int64_t>& size) {
    if (size.empty()) {
        diopiCastDtype(ctx, const_cast<diopiTensorHandle_t>(out.tensorHandle()), const_cast<diopiTensorHandle_t>(input.tensorHandle()));
        return diopiSuccess;
    }
    // Avoid modifying the input tensor (when input == out).
    AscendTensor tmp = out;
    if (!out.defined() || input.isSame(out)) {
        AscendTensor tmp1;
        makeTensor(ctx, tmp1, size, input.dtype());
        tmp = tmp1;
    }
    auto ptr = const_cast<diopiTensorHandle_t>(tmp.tensorHandle());
    AclOpRunner<2, 1>("BroadcastTo", ctx).addInput(input).addConstInput(size).addOutput(ptr).run();
    out = AscendTensor(ptr);
    return diopiSuccess;
}

std::vector<int64_t> inferSize(const std::vector<int64_t>& shape1, const std::vector<int64_t>& shape2) {
    size_t dimsA = shape1.size();
    size_t dimsB = shape2.size();
    size_t ndim = dimsA > dimsB ? dimsA : dimsB;
    std::vector<int64_t> expandedSizes(ndim);

    // Use ptrdiff_t to ensure signed comparison.
    for (ptrdiff_t i = (ptrdiff_t)ndim - 1; i >= 0; --i) {
        ptrdiff_t offset = ndim - 1 - i;
        ptrdiff_t dimA = dimsA - 1 - offset;
        ptrdiff_t dimB = dimsB - 1 - offset;
        auto sizeA = (dimA >= 0) ? shape1[dimA] : 1;
        auto sizeB = (dimB >= 0) ? shape2[dimB] : 1;

        // 1s map to the other size (even 0).
        expandedSizes[i] = sizeA == 1 ? sizeB : sizeA;
    }

    return expandedSizes;
}

diopiTensorHandle_t hostToDevice(diopiContextHandle_t ctx, diopiConstTensorHandle_t src) {
    diopiDevice_t device;
    diopiGetTensorDevice(src, &device);
    if (device == diopi_host) {
        diopiTensorHandle_t dst;
        diopiSize_t size, stride;
        diopiDtype_t dtype;
        diopiGetTensorShape(src, &size);
        diopiGetTensorStride(src, &stride);
        diopiGetTensorDtype(src, &dtype);
        diopiRequireTensor(ctx, &dst, &size, &stride, dtype, diopi_device);
        const void* srcPtr;
        void* dstPtr;
        diopiGetTensorDataConst(src, &srcPtr);
        diopiGetTensorData(dst, &dstPtr);
        diopiStreamHandle_t stream;
        diopiGetStream(ctx, &stream);
        int64_t elemsize = getBaseBufferSize(src);
        CALL_ACLRT(aclrtMemcpyAsync(dstPtr, elemsize, const_cast<void*>(srcPtr), elemsize, ACL_MEMCPY_HOST_TO_DEVICE, stream));
        CALL_ACLRT(aclrtSynchronizeStream(stream));
        return dst;
    } else {
        return const_cast<diopiTensorHandle_t>(src);
    }
}
}  // namespace ascend
}  // namespace impl
