/* NDArray.cpp
 *
 * NDArray classes
 *
 *
 * \author Mark Rivers
 *
 * \author University of Chicago
 *
 * \date May 11 2008
 *
 */

#include <string.h>
#include <stdio.h>
#include <ellLib.h>

#include <epicsMutex.h>
#include <epicsTypes.h>
#include <epicsString.h>
#include <ellLib.h>
#include <cantProceed.h>

#include "NDArray.h"

static const char *driverName = "NDArray";

/** NDArrayPool constructor
  * \param[in] maxBuffers Maximum number of NDArray objects that the pool is allowed to contain; -1=unlimited.
  * \param[in] maxMemory Maxiumum number of bytes of memory the the pool is allowed to use, summed over
  * all of the NDArray objects; -1=unlimited.
  */
NDArrayPool::NDArrayPool(int maxBuffers, size_t maxMemory)
    : maxBuffers(maxBuffers), numBuffers(0), maxMemory(maxMemory), memorySize(0), numFree(0)
{
    ellInit(&this->freeList);
    this->listLock = epicsMutexCreate();
}

/** Allocates a new NDArray object; the first 3 arguments are required.
  * \param[in] ndims The number of dimensions in the NDArray. 
  * \param[in] dims Array of dimensions, whose size must be at least ndims.
  * \param[in] dataType Data type of the NDArray data.
  * \param[in] dataSize Number of bytes to allocate for the array data; if 0 then
  * alloc() will compute the size required from ndims, dims, and dataType.
  * \param[in] pData Pointer to a data buffer; if NULL then alloc will allocate a new
  * array buffer; if not NULL then it is assumed to point to a valid buffer.
  * 
  * If pData is not NULL then dataSize must contain the actual number of bytes in the existing
  * array, and this array must be large enough to hold the array data. 
  * alloc() searches
  * its free list to find a free NDArray buffer. If is cannot find one then it will
  * allocate a new one and add it to the free list. If doing so would exceed maxBuffers
  * then alloc() will return an error. Similarly if allocating the memory required for
  * this NDArray would cause the cumulative memory allocated for the pool to exceed
  * maxMemory then an error will be returned. alloc() sets the reference count for the
  * returned NDArray to 1.
  */
NDArray* NDArrayPool::alloc(int ndims, int *dims, NDDataType_t dataType, int dataSize, void *pData)
{
    NDArray *pArray;
    NDArrayInfo_t arrayInfo;
    int i;
    const char* functionName = "NDArrayPool::alloc:";

    epicsMutexLock(this->listLock);

    /* Find a free image */
    pArray = (NDArray *)ellFirst(&this->freeList);

    if (!pArray) {
        /* We did not find a free image.
         * Allocate a new one if we have not exceeded the limit */
        if ((this->maxBuffers > 0) && (this->numBuffers >= this->maxBuffers)) {
            printf("%s: error: reached limit of %d buffers (memory use=%d/%d bytes)\n",
                   functionName, this->maxBuffers, this->memorySize, this->maxMemory);
        } else {
            this->numBuffers++;
            pArray = new NDArray;
            ellAdd(&this->freeList, &pArray->node);
            this->numFree++;
        }
    }

    if (pArray) {
        /* We have a frame */
        /* Initialize fields */
        pArray->owner = this;
        pArray->dataType = dataType;
        pArray->ndims = ndims;
        memset(pArray->dims, 0, sizeof(pArray->dims));
        for (i=0; i<ndims && i<ND_ARRAY_MAX_DIMS; i++) {
            pArray->dims[i].size = dims[i];
            pArray->dims[i].offset = 0;
            pArray->dims[i].binning = 1;
            pArray->dims[i].reverse = 0;
        }
        pArray->getInfo(&arrayInfo);
        if (dataSize == 0) dataSize = arrayInfo.totalBytes;
        if (arrayInfo.totalBytes > dataSize) {
            printf("%s: ERROR: required size=%d passed size=%d is too small\n",
            functionName, arrayInfo.totalBytes, dataSize);
            pArray=NULL;
        }
    }

    if (pArray) {
        /* If the caller passed a valid buffer use that, trust that its size is correct */
        if (pData) {
            pArray->pData = pData;
        } else {
            /* See if the current buffer is big enough */
            if (pArray->dataSize < dataSize) {
                /* No, we need to free the current buffer and allocate a new one */
                /* See if there is enough room */
                this->memorySize -= pArray->dataSize;
                if (pArray->pData) {
                    free(pArray->pData);
                    pArray->pData = NULL;
                }
                if ((this->maxMemory > 0) && ((this->memorySize + dataSize) > this->maxMemory)) {
                    printf("%s: error: reached limit of %d memory (%d/%d buffers)\n",
                           functionName, this->maxMemory, this->numBuffers, this->maxBuffers);
                    pArray = NULL;
                } else {
                    pArray->pData = callocMustSucceed(dataSize, 1,
                                                      functionName);
                    pArray->dataSize = dataSize;
                    this->memorySize += dataSize;
                }
            }
        }
    }
    if (pArray) {
        /* Set the reference count to 1, remove from free list */
        pArray->referenceCount = 1;
        ellDelete(&this->freeList, &pArray->node);
        this->numFree--;
    }
    epicsMutexUnlock(this->listLock);
    return (pArray);
}

/** This method makes a copy of an NDArray object.
  * \param[in] pIn The input array to be copied.
  * \param[in] pOut The output array that will be copied to.
  * \param[in] copyData If this flag is 1 then everything including the array data is copied;
  * if 0 then everything except the data (including attributes) is copied.
  * \return Returns a pointer to the output array.
  *
  * If pOut is NULL then it is first allocated. If the output array
  * object already exists (pOut!=NULL) then it must have sufficient memory allocated to
  * it to hold the data.
  */
NDArray* NDArrayPool::copy(NDArray *pIn, NDArray *pOut, int copyData)
{
    //const char *functionName = "copy";
    int dimSizeOut[ND_ARRAY_MAX_DIMS];
    int i;
    int numCopy;
    NDArrayInfo arrayInfo;

    /* If the output array does not exist then create it */
    if (!pOut) {
        for (i=0; i<pIn->ndims; i++) dimSizeOut[i] = pIn->dims[i].size;
        pOut = this->alloc(pIn->ndims, dimSizeOut, pIn->dataType, 0, NULL);
    }
    pOut->uniqueId = pIn->uniqueId;
    pOut->timeStamp = pIn->timeStamp;
    pOut->ndims = pIn->ndims;
    memcpy(pOut->dims, pIn->dims, sizeof(pIn->dims));
    pOut->dataType = pIn->dataType;
    if (copyData) {
        pIn->getInfo(&arrayInfo);
        numCopy = arrayInfo.totalBytes;
        if (pOut->dataSize < numCopy) numCopy = pOut->dataSize;
        memcpy(pOut->pData, pIn->pData, numCopy);
    }
    pOut->clearAttributes();
    pIn->copyAttributes(pOut);
    return(pOut);
}

/** This method increases the reference count for the NDArray object.
  * \param[in] pArray The array on which to increase the reference count.
  *
  * Plugins must call reserve() when an NDArray is placed on a queue for later
  * processing.
  */
int NDArrayPool::reserve(NDArray *pArray)
{
    const char *functionName = "reserve";

    /* Make sure we own this array */
    if (pArray->owner != this) {
        printf("%s:%s: ERROR, not owner!  owner=%p, should be this=%p\n",
               driverName, functionName, pArray->owner, this);
        return(ND_ERROR);
    }
    epicsMutexLock(this->listLock);
    pArray->referenceCount++;
    epicsMutexUnlock(this->listLock);
    return ND_SUCCESS;
}

/** This method decreases the reference count for the NDArray object.
  * \param[in] pArray The array on which to decrease the reference count.
  *
  * When the reference count reaches 0 the NDArray is placed back in the free list.
  * Plugins must call release() when an NDArray is removed from the queue and
  * processing on it is complete. Drivers must call release() after calling all
  * plugins.
  */
int NDArrayPool::release(NDArray *pArray)
{
    const char *functionName = "release";

    /* Make sure we own this array */
    if (pArray->owner != this) {
        printf("%s:%s: ERROR, not owner!  owner=%p, should be this=%p\n",
               driverName, functionName, pArray->owner, this);
        return(ND_ERROR);
    }
    epicsMutexLock(this->listLock);
    pArray->referenceCount--;
    if (pArray->referenceCount == 0) {
        /* The last user has released this image, add it back to the free list */
        ellAdd(&this->freeList, &pArray->node);
        this->numFree++;
    }
    if (pArray->referenceCount < 0) {
        printf("%s:release ERROR, reference count < 0 pArray=%p\n",
            driverName, pArray);
    }
    epicsMutexUnlock(this->listLock);
    return ND_SUCCESS;
}

template <typename dataTypeIn, typename dataTypeOut> void convertType(NDArray *pIn, NDArray *pOut)
{
    int i;
    dataTypeIn *pDataIn = (dataTypeIn *)pIn->pData;
    dataTypeOut *pDataOut = (dataTypeOut *)pOut->pData;
    NDArrayInfo_t arrayInfo;

    pOut->getInfo(&arrayInfo);
    for (i=0; i<arrayInfo.nElements; i++) {
        *pDataOut++ = (dataTypeOut)(*pDataIn++);
    }
}

template <typename dataTypeOut> int convertTypeSwitch (NDArray *pIn, NDArray *pOut)
{
    int status = ND_SUCCESS;

    switch(pIn->dataType) {
        case NDInt8:
            convertType<epicsInt8, dataTypeOut> (pIn, pOut);
            break;
        case NDUInt8:
            convertType<epicsUInt8, dataTypeOut> (pIn, pOut);
            break;
        case NDInt16:
            convertType<epicsInt16, dataTypeOut> (pIn, pOut);
            break;
        case NDUInt16:
            convertType<epicsUInt16, dataTypeOut> (pIn, pOut);
            break;
        case NDInt32:
            convertType<epicsInt32, dataTypeOut> (pIn, pOut);
            break;
        case NDUInt32:
            convertType<epicsUInt32, dataTypeOut> (pIn, pOut);
            break;
        case NDFloat32:
            convertType<epicsFloat32, dataTypeOut> (pIn, pOut);
            break;
        case NDFloat64:
            convertType<epicsFloat64, dataTypeOut> (pIn, pOut);
            break;
        default:
            status = ND_ERROR;
            break;
    }
    return(status);
}


template <typename dataTypeIn, typename dataTypeOut> void convertDim(NDArray *pIn, NDArray *pOut,
                                                                void *pDataIn, void *pDataOut, int dim)
{
    dataTypeOut *pDOut = (dataTypeOut *)pDataOut;
    dataTypeIn *pDIn = (dataTypeIn *)pDataIn;
    NDDimension_t *pOutDims = pOut->dims;
    NDDimension_t *pInDims = pIn->dims;
    int inStep, outStep, inOffset, inDir;
    int i, inc, in, out, bin;

    inStep = 1;
    outStep = 1;
    inDir = 1;
    inOffset = pOutDims[dim].offset;
    for (i=0; i<dim; i++) {
        inStep  *= pInDims[i].size;
        outStep *= pOutDims[i].size;
    }
    if (pOutDims[dim].reverse) {
        inOffset += pOutDims[dim].size * pOutDims[dim].binning - 1;
        inDir = -1;
    }
    inc = inDir * inStep;
    pDIn += inOffset*inStep;
    for (in=0, out=0; out<pOutDims[dim].size; out++, in++) {
        for (bin=0; bin<pOutDims[dim].binning; bin++) {
            if (dim > 0) {
                convertDim <dataTypeIn, dataTypeOut> (pIn, pOut, pDIn, pDOut, dim-1);
            } else {
                *pDOut += (dataTypeOut)*pDIn;
            }
            pDIn += inc;
        }
        pDOut += outStep;
    }
}

template <typename dataTypeOut> int convertDimensionSwitch(NDArray *pIn, NDArray *pOut,
                                                            void *pDataIn, void *pDataOut, int dim)
{
    int status = ND_SUCCESS;

    switch(pIn->dataType) {
        case NDInt8:
            convertDim <epicsInt8, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDUInt8:
            convertDim <epicsUInt8, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDInt16:
            convertDim <epicsInt16, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDUInt16:
            convertDim <epicsUInt16, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDInt32:
            convertDim <epicsInt32, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDUInt32:
            convertDim <epicsUInt32, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDFloat32:
            convertDim <epicsFloat32, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDFloat64:
            convertDim <epicsFloat64, dataTypeOut> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        default:
            status = ND_ERROR;
            break;
    }
    return(status);
}

static int convertDimension(NDArray *pIn,
                                  NDArray *pOut,
                                  void *pDataIn,
                                  void *pDataOut,
                                  int dim)
{
    int status = ND_SUCCESS;
    /* This routine is passed:
     * A pointer to the start of the input data
     * A pointer to the start of the output data
     * An array of dimensions
     * A dimension index */
    switch(pOut->dataType) {
        case NDInt8:
            convertDimensionSwitch <epicsInt8>(pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDUInt8:
            convertDimensionSwitch <epicsUInt8> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDInt16:
            convertDimensionSwitch <epicsInt16> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDUInt16:
            convertDimensionSwitch <epicsUInt16> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDInt32:
            convertDimensionSwitch <epicsInt32> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDUInt32:
            convertDimensionSwitch <epicsUInt32> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDFloat32:
            convertDimensionSwitch <epicsFloat32> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        case NDFloat64:
            convertDimensionSwitch <epicsFloat64> (pIn, pOut, pDataIn, pDataOut, dim);
            break;
        default:
            status = ND_ERROR;
            break;
    }
    return(status);
}

/** Creates a new output NDArray from an input NDArray, performing
  * conversion operations.
  * The conversion can change the data type if dataTypeOut is different from
  * pIn->dataType. It can also change the dimensions. outDims may have different
  * values of size, binning, offset and reverse for each of its dimensions from input
  * array dimensions (pIn->dims).
  */
int NDArrayPool::convert(NDArray *pIn,
                         NDArray **ppOut,
                         NDDataType_t dataTypeOut,
                         NDDimension_t *dimsOut)
{
    int dimsUnchanged;
    int dimSizeOut[ND_ARRAY_MAX_DIMS];
    NDDimension_t dimsOutCopy[ND_ARRAY_MAX_DIMS];
    int i;
    int status = ND_SUCCESS;
    NDArray *pOut;
    NDArrayInfo_t arrayInfo;
    NDAttribute *pAttribute;
    int colorMode, colorModeMono = NDColorModeMono;
    const char *functionName = "convert";

    /* Initialize failure */
    *ppOut = NULL;

    /* Copy the input dimension array because we need to modify it
     * but don't want to affect caller */
    memcpy(dimsOutCopy, dimsOut, pIn->ndims*sizeof(NDDimension_t));
    /* Compute the dimensions of the output array */
    dimsUnchanged = 1;
    for (i=0; i<pIn->ndims; i++) {
        dimsOutCopy[i].size = dimsOutCopy[i].size/dimsOutCopy[i].binning;
        if (dimsOutCopy[i].size <= 0) {
            printf("%s:%s: ERROR, invalid output dimension, size=%d, binning=%d\n",
                driverName, functionName, dimsOut[i].size, dimsOut[i].binning);
            return(ND_ERROR);
        }
        dimSizeOut[i] = dimsOutCopy[i].size;
        if ((pIn->dims[i].size  != dimsOutCopy[i].size) ||
            (dimsOutCopy[i].offset != 0) ||
            (dimsOutCopy[i].binning != 1) ||
            (dimsOutCopy[i].reverse != 0)) dimsUnchanged = 0;
    }

    /* We now know the datatype and dimensions of the output array.
     * Allocate it */
    pOut = alloc(pIn->ndims, dimSizeOut, dataTypeOut, 0, NULL);
    *ppOut = pOut;
    if (!pOut) {
        printf("%s:%s: ERROR, cannot allocate output array\n",
            driverName, functionName);
        return(ND_ERROR);
    }
    /* Copy fields from input to output */
    pOut->timeStamp = pIn->timeStamp;
    pOut->uniqueId = pIn->uniqueId;
    /* Replace the dimensions with those passed to this function */
    memcpy(pOut->dims, dimsOutCopy, pIn->ndims*sizeof(NDDimension_t));
    pIn->copyAttributes(pOut);

    pOut->getInfo(&arrayInfo);

    if (dimsUnchanged) {
        if (pIn->dataType == pOut->dataType) {
            /* The dimensions are the same and the data type is the same,
             * then just copy the input image to the output image */
            memcpy(pOut->pData, pIn->pData, arrayInfo.totalBytes);
            return ND_SUCCESS;
        } else {
            /* We need to convert data types */
            switch(pOut->dataType) {
                case NDInt8:
                    convertTypeSwitch <epicsInt8> (pIn, pOut);
                    break;
                case NDUInt8:
                    convertTypeSwitch <epicsUInt8> (pIn, pOut);
                    break;
                case NDInt16:
                    convertTypeSwitch <epicsInt16> (pIn, pOut);
                    break;
                case NDUInt16:
                    convertTypeSwitch <epicsUInt16> (pIn, pOut);
                    break;
                case NDInt32:
                    convertTypeSwitch <epicsInt32> (pIn, pOut);
                    break;
                case NDUInt32:
                    convertTypeSwitch <epicsUInt32> (pIn, pOut);
                    break;
                case NDFloat32:
                    convertTypeSwitch <epicsFloat32> (pIn, pOut);
                    break;
                case NDFloat64:
                    convertTypeSwitch <epicsFloat64> (pIn, pOut);
                    break;
                default:
                    status = ND_ERROR;
                    break;
            }
        }
    } else {
        /* The input and output dimensions are not the same, so we are extracting a region
         * and/or binning */
        /* Clear entire output array */
        memset(pOut->pData, 0, arrayInfo.totalBytes);
        status = convertDimension(pIn, pOut, pIn->pData, pOut->pData, pIn->ndims-1);
    }

    /* Set fields in the output array */
    for (i=0; i<pIn->ndims; i++) {
        pOut->dims[i].offset = pIn->dims[i].offset + dimsOutCopy[i].offset;
        pOut->dims[i].binning = pIn->dims[i].binning * dimsOutCopy[i].binning;
        if (pIn->dims[i].reverse) pOut->dims[i].reverse = !pOut->dims[i].reverse;
    }

    /* If the frame is an RGBx frame and we have collapsed that dimension then change the colorMode */
    pAttribute = pOut->findAttribute("ColorMode");
    if (pAttribute && pAttribute->getValue(NDAttrInt32, &colorMode)) {
        if      ((colorMode == NDColorModeRGB1) && (pOut->dims[0].size != 3)) 
                pAttribute->setValue(NDAttrInt32, (void *)&colorModeMono);
        else if ((colorMode == NDColorModeRGB2) && (pOut->dims[1].size != 3)) 
                pAttribute->setValue(NDAttrInt32, (void *)&colorModeMono);
        else if ((colorMode == NDColorModeRGB3) && (pOut->dims[2].size != 3))
                pAttribute->setValue(NDAttrInt32, (void *)&colorModeMono);
    }
    return ND_SUCCESS;
}


/** Reports on the free list size and other properties of the NDArrayPool
  * object.
  */
int NDArrayPool::report(int details)
{
    printf("NDArrayPool:\n");
    printf("  numBuffers=%d, maxBuffers=%d\n",
        this->numBuffers, this->maxBuffers);
    printf("  memorySize=%d, maxMemory=%d\n",
        this->memorySize, this->maxMemory);
    printf("  numFree=%d\n",
        this->numFree);
        
    return ND_SUCCESS;
}

/** NDArray constructor, no parameters.
  * Initializes all fields to 0.  Creates the attribute linked list and linked list mutex. */
NDArray::NDArray()
    : referenceCount(0), owner(NULL),
      uniqueId(0), timeStamp(0.0), ndims(0), dataType(NDInt8),
      dataSize(0),  pData(NULL)
{
    memset(this->dims, 0, sizeof(this->dims));
    memset(&this->node, 0, sizeof(this->node));
    ellInit(&this->attributeList);
    this->listLock = epicsMutexCreate();
}

/** NDArray destructor 
  * Frees the data array, deletes all attributes, frees the attribute list and destroys the mutex. */
NDArray::~NDArray()
{
    if (this->pData) free(this->pData);
    this->clearAttributes();
    ellFree(&this->attributeList);
    epicsMutexDestroy(this->listLock);
}

/** Convenience method returns information about an NDArray, including the total number of elements, 
  * the number of bytes per element, and the total number of bytes in the array.
  \param[out] pInfo Pointer to an NDArrayInfo_t structure, must have been allocated by caller. */
int NDArray::getInfo(NDArrayInfo_t *pInfo)
{
    int i;

    switch(this->dataType) {
        case NDInt8:
            pInfo->bytesPerElement = sizeof(epicsInt8);
            break;
        case NDUInt8:
            pInfo->bytesPerElement = sizeof(epicsUInt8);
            break;
        case NDInt16:
            pInfo->bytesPerElement = sizeof(epicsInt16);
            break;
        case NDUInt16:
            pInfo->bytesPerElement = sizeof(epicsUInt16);
            break;
        case NDInt32:
            pInfo->bytesPerElement = sizeof(epicsInt32);
            break;
        case NDUInt32:
            pInfo->bytesPerElement = sizeof(epicsUInt32);
            break;
        case NDFloat32:
            pInfo->bytesPerElement = sizeof(epicsFloat32);
            break;
        case NDFloat64:
            pInfo->bytesPerElement = sizeof(epicsFloat64);
            break;
        default:
            return(ND_ERROR);
            break;
    }
    pInfo->nElements = 1;
    for (i=0; i<this->ndims; i++) pInfo->nElements *= this->dims[i].size;
    pInfo->totalBytes = pInfo->nElements * pInfo->bytesPerElement;
    return(ND_SUCCESS);
}

/** Initializes the dimension structure to size=size, binning=1, reverse=0, offset=0.
  * \param[in] pDimension Pointer to an NDDimension_t structure, must have been allocated by caller.
  * \param[in] size The size of this dimension. */
int NDArray::initDimension(NDDimension_t *pDimension, int size)
{
    pDimension->size=size;
    pDimension->binning = 1;
    pDimension->offset = 0;
    pDimension->reverse = 0;
    return ND_SUCCESS;
}

/** Calls NDArrayPool->reserve() for this NDArray object; increases the reference count for this array. */
int NDArray::reserve()
{
    const char *functionName = "NDArray::reserve";

    NDArrayPool *pNDArrayPool = (NDArrayPool *)this->owner;

    if (!pNDArrayPool) {
        printf("%s: ERROR, no owner\n", functionName);
        return(ND_ERROR);
    }
    return(pNDArrayPool->reserve(this));
}

/** Calls NDArrayPool->release() for this object; decreases the reference count for this array. */
int NDArray::release()
{
    const char *functionName = "NDArray::release";

    NDArrayPool *pNDArrayPool = (NDArrayPool *)this->owner;

    if (!pNDArrayPool) {
        printf("%s: ERROR, no owner\n", functionName);
        return(ND_ERROR);
    }
    return(pNDArrayPool->release(this));
}

/** Adds an attribute to the array.
  * \param[in] pName The name of the attribute to be added.
  * \return Returns a pointer to the attribute.
  *
  * Searches for an existing attribute of this name.  If found it just returns the pointer.
  * If not found it creates a new attribute with this name and adds it to the attribute
  * list for this array. */
NDAttribute* NDArray::addAttribute(const char *pName)
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = this->findAttribute(pName);
    if (!pAttribute) {
        pAttribute = new NDAttribute(pName);
        ellAdd(&this->attributeList, &pAttribute->node);
    }
    return(pAttribute);
}

/** Adds an attribute to the array.
  * \param[in] pName The name of the attribute to be added.
  * \param[in] dataType The data type of the attribute to be added.
  * \param[in] pValue A pointer to the value for this attribute.
  * \return Returns a pointer to the attribute.
  *
  * Searches for an existing attribute of this name.  If found it sets the data type and
  * value to those passed to this method.
  * If not found it creates a new attribute with this name, adds it to the attribute
  * list for this array and sets the data type and value. */
NDAttribute* NDArray::addAttribute(const char *pName, NDAttrDataType_t dataType, void *pValue)
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = this->addAttribute(pName);
    pAttribute->setValue(dataType, pValue);
    return(pAttribute);
}

/** Adds an attribute to the array.
  * \param[in] pName The name of the attribute to be added.
  * \param[in] pDescription The description of the attribute to be added.
  * \param[in] dataType The data type of the attribute to be added.
  * \param[in] pValue A pointer to the value for this attribute.
  * \return Returns a pointer to the attribute.
  *
  * Searches for an existing attribute of this name.  If found it sets the description, data type and
  * value to those passed to this method.
  * If not found it creates a new attribute with this name, adds it to the attribute
  * list for this array and sets the description, data type and value. */
NDAttribute* NDArray::addAttribute(const char *pName, const char *pDescription, NDAttrDataType_t dataType, void *pValue)
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = this->addAttribute(pName);
    pAttribute->setDescription(pDescription);
    pAttribute->setValue(dataType, pValue);
    return(pAttribute);
}

/** Adds an attribute to the array.
  * \param[in] pIn A pointer to an existing attribute from which values will be copied.
  * \return Returns a pointer to the attribute.
  *
  * Searches for an existing attribute of this name.  If found it sets the description, data type and
  * value to those passed to this method.
  * If not found it creates a new attribute with this name, adds it to the attribute
  * list for this array and sets the description, data type and value. */
NDAttribute* NDArray::addAttribute(NDAttribute *pIn)
{
    NDAttribute *pAttribute;
    void *pValue = &pIn->value;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = this->addAttribute(pIn->pName);
    pAttribute->setDescription(pIn->pDescription);
    if (pIn->dataType == NDAttrString) pValue = pIn->pString;
    pAttribute->setValue(pIn->dataType, pValue);
    return(pAttribute);
}

/** Finds an attribute by name.
  * \param[in] pName The name of the attribute to be found.
  * \return Returns a pointer to the attribute if found, NULL if not found. 
  *
  * The search is case-insensitive.*/
NDAttribute* NDArray::findAttribute(const char *pName)
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = (NDAttribute *)ellFirst(&this->attributeList);
    while (pAttribute) {
        if (epicsStrCaseCmp(pAttribute->pName, pName) == 0) return(pAttribute);
        pAttribute = (NDAttribute *)ellNext(&pAttribute->node);
    }
    return(NULL);
}

/** Finds the next attribute in the NDArray linked list of attributes.
  * \param[in] pAttributeIn A pointer to the previous attribute in the list; 
  * if NULL the first attribute in the list is returned.
  * \return Returns a pointer to the next attribute if there is one, 
  * NULL if there are no more attributes in the list. */
NDAttribute* NDArray::nextAttribute(NDAttribute *pAttributeIn)
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    if (!pAttributeIn) pAttribute = (NDAttribute *)ellFirst(&this->attributeList);
    else pAttribute = (NDAttribute *)ellNext(&pAttributeIn->node);
    return(pAttribute);
}

/** Returns the total number of attributes in the NDArray linked list of attributes.
  * \return Returns the number of attributes. */
int NDArray::numAttributes()
{
    //const char *functionName = "NDArray::addAttribute";

    return ellCount(&this->attributeList);
}

/** Deletes an attribute from the array.
  * \param[in] pName The name of the attribute to be deleted.
  * \return Returns ND_SUCCESS if the attribute was found and deleted, ND_ERROR if the
  * attribute was not found. */
int NDArray::deleteAttribute(const char *pName)
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = this->findAttribute(pName);
    if (!pAttribute) return(ND_ERROR);
    ellDelete(&this->attributeList, &pAttribute->node);
    delete pAttribute;
    return(ND_SUCCESS);
}

/** Deletes all attributes from the array. */
int NDArray::clearAttributes()
{
    NDAttribute *pAttribute;
    //const char *functionName = "NDArray::addAttribute";

    pAttribute = (NDAttribute *)ellFirst(&this->attributeList);
    while (pAttribute) {
        ellDelete(&this->attributeList, &pAttribute->node);
        delete pAttribute;
        pAttribute = (NDAttribute *)ellFirst(&this->attributeList);
    }
    return(ND_SUCCESS);
}

/** Copies all attributes from the array to an output NDArray.
  * \param[out] pOut A pointer to the output array to copy the attributes to.
  *
  * The attributes are added to any existing attributes already present in the output array. */
int NDArray::copyAttributes(NDArray *pOut)
{
    NDAttribute *pAttribute;
    void *pValue;
    //const char *functionName = "NDArray::copyAttributes";

    pAttribute = (NDAttribute *)ellFirst(&this->attributeList);
    while (pAttribute) {
        if (pAttribute->dataType == NDAttrString) 
            pValue = pAttribute->pString; 
        else 
            pValue = &pAttribute->value;
        pOut->addAttribute(pAttribute->pName, pAttribute->pDescription, pAttribute->dataType, pValue);
        pAttribute = (NDAttribute *)ellNext(&pAttribute->node);
    }
    return(ND_SUCCESS);
}

/** Reports on the properties of the array.
  */
int NDArray::report(int details)
{
    NDAttribute *pAttr;
    int dim;
    
    printf("\n");
    printf("NDArrayArray address=%p:\n", this);
    printf("  ndims=%d dims=[",
        this->ndims);
    for (dim=0; dim<this->ndims; dim++) printf("%d ", this->dims[dim].size);
    printf("]\n");
    printf("  dataType=%d, dataSize=%d, pData=%p\n",
        this->dataType, this->dataSize, this->pData);
    printf("  uniqueId=%d, timeStamp=%f\n",
        this->uniqueId, this->timeStamp);
    printf("  number of attributes=%d\n", ellCount(&this->attributeList));
    if (details > 5) {
        pAttr = (NDAttribute *) ellFirst(&this->attributeList);
        while (pAttr) {
            pAttr->report(details);
            pAttr = (NDAttribute *) ellNext(&pAttr->node);
        }
    }
    return ND_SUCCESS;
}


/** NDAttribute constructor 
  * \param[in] pName The name of the attribute to be created. 
  *
  * Sets the attribute name to pName, the description and pString to NULL, and the data type to NDAttrUndefined. */
NDAttribute::NDAttribute(const char *pName)
{
    this->pName = epicsStrDup(pName);
    this->pDescription = NULL;
    this->pString = NULL;
    this->dataType = NDAttrUndefined;
}

/** NDAttribute destructor 
  * Frees the strings for the name, and if they exist, the description and pString. */
NDAttribute::~NDAttribute()
{
    if (this->pName) free(this->pName);
    if (this->pDescription) free(this->pDescription);
    if (this->pString) free(this->pString);
}

/** Returns the length of the name string including 0 terminator for this attribute. */
int NDAttribute::getNameInfo(size_t *pNameSize) {

    *pNameSize = strlen(this->pName)+1;
    return(ND_SUCCESS);
}

/** Returns the name string for this attribute. 
  * \param[out] pName String to hold the name. 
  * \param[in] nameSize Maximum size for the name string; 
    if 0 then pName is assumed to be big enough to hold the name string plus 0 terminator. */
int NDAttribute::getName(char *pName, size_t nameSize) {

    if (nameSize == 0) nameSize = strlen(this->pName)+1;
    strncpy(pName, this->pName, nameSize);
    return(ND_SUCCESS);
}

/** Returns the length of the description string including 0 terminator for this attribute. */
int NDAttribute::getDescriptionInfo(size_t *pDescSize) {

    if (this->pDescription)
        *pDescSize = strlen(this->pDescription)+1;
    else
        *pDescSize = 0;
    return(ND_SUCCESS);
}

/** Returns the description string for this attribute. 
  * \param[out] pDescription String to hold the desciption. 
  * \param[in] descSize Maximum size for the description string; 
    if 0 then pDescription is assumed to be big enough to hold the description string plus 0 terminator. */
int NDAttribute::getDescription(char *pDescription, size_t descSize) {

    if (this->pDescription) {
        if (descSize == 0) descSize = strlen(this->pDescription)+1;
        strncpy(pDescription, this->pDescription, descSize);
    } else
        *pDescription = NULL;
    return(ND_SUCCESS);
}

/** Sets the description string for this attribute. 
  * \param[in] pDescription String with the desciption. */
int NDAttribute::setDescription(const char *pDescription) {

    if (this->pDescription) {
        /* If the new description is the same as the old one return, 
         * saves freeing and allocating memory */
        if (strcmp(this->pDescription, pDescription) == 0) return(ND_SUCCESS);
        free(this->pDescription);
    }
    if (pDescription) this->pDescription = epicsStrDup(pDescription);
    else this->pDescription = NULL;
    return(ND_SUCCESS);
}

/** Sets the value for this attribute. 
  * \param[in] dataType Data type of the value.
  * \param[in] pValue Pointer to the value. */
int NDAttribute::setValue(NDAttrDataType_t dataType, void *pValue)
{
    NDAttrDataType_t prevDataType = this->dataType;
        
    this->dataType = dataType;

    /* If any data type but undefined then pointer must be valid */
    if ((dataType != NDAttrUndefined) && !pValue) return(ND_ERROR);

    /* Treat strings specially */
    if (dataType == NDAttrString) {
        /* If the previous value was the same string don't do anything, 
         * saves freeing and allocating memory.  
         * If not the same free the old string and copy new one. */
        if ((prevDataType == NDAttrString) && this->pString) {
            if (strcmp(this->pString, (char *)pValue) == 0) return(ND_SUCCESS);
            free(this->pString);
        }
        this->pString = epicsStrDup((char *)pValue);
        return(ND_SUCCESS);
    }
    if (this->pString) {
        free(this->pString);
        this->pString = NULL;
    }
    switch (dataType) {
        case NDAttrInt8:
            this->value.i8 = *(epicsInt8 *)pValue;
            break;
        case NDAttrUInt8:
            this->value.ui8 = *(epicsUInt8 *)pValue;
            break;
        case NDAttrInt16:
            this->value.i16 = *(epicsInt16 *)pValue;
            break;
        case NDAttrUInt16:
            this->value.ui16 = *(epicsUInt16 *)pValue;
            break;
        case NDAttrInt32:
            this->value.i32 = *(epicsInt32*)pValue;
            break;
        case NDAttrUInt32:
            this->value.ui32 = *(epicsUInt32 *)pValue;
            break;
        case NDAttrFloat32:
            this->value.f32 = *(epicsFloat32 *)pValue;
            break;
        case NDAttrFloat64:
            this->value.f64 = *(epicsFloat64 *)pValue;
            break;
        case NDAttrUndefined:
            break;
        default:
            return(ND_ERROR);
            break;
    }
    return(ND_SUCCESS);
}

/** Returns the data type and size of this attribute.
  * \param[out] pDataType Pointer to location to return the data type.
  * \param[out] pSize Pointer to location to return the data size; this is the
  *  data type size for all data types except NDAttrString, in which case it is the length of the
  * string including 0 terminator. */
int NDAttribute::getValueInfo(NDAttrDataType_t *pDataType, size_t *pSize)
{
    *pDataType = this->dataType;
    switch (this->dataType) {
        case NDAttrInt8:
            *pSize = sizeof(this->value.i8);
            break;
        case NDAttrUInt8:
            *pSize = sizeof(this->value.ui8);
            break;
        case NDAttrInt16:
            *pSize = sizeof(this->value.i16);
            break;
        case NDAttrUInt16:
            *pSize = sizeof(this->value.ui16);
            break;
        case NDAttrInt32:
            *pSize = sizeof(this->value.i32);
            break;
        case NDAttrUInt32:
            *pSize = sizeof(this->value.ui32);
            break;
        case NDAttrFloat32:
            *pSize = sizeof(this->value.f32);
            break;
        case NDAttrFloat64:
            *pSize = sizeof(this->value.f64);
            break;
        case NDAttrString:
            if (this->pString) *pSize = strlen(this->pString)+1;
            else *pSize = 0;
            break;
        case NDAttrUndefined:
            *pSize = 0;
            break;
        default:
            return(ND_ERROR);
            break;
    }
    return(ND_SUCCESS);
}

/** Returns the value of this attribute.
  * \param[in] dataType Data type for the value.
  * \param[out] pValue Pointer to location to return the value.
  * \param[in] dataSize Size of the input data location; only used when dataType is NDAttrString.
  *
  * Currently the dataType parameter is only used to check that it matches the actual data type,
  * and ND_ERROR is returned if it does not.  In the future data type conversion may be added. */
int NDAttribute::getValue(NDAttrDataType_t dataType, void *pValue, size_t dataSize)
{
    if (dataType != this->dataType) return(ND_ERROR);
    switch (this->dataType) {
        case NDAttrInt8:
            *(epicsInt8 *)pValue = this->value.i8;
            break;
        case NDAttrUInt8:
             *(epicsUInt8 *)pValue = this->value.ui8;
            break;
        case NDAttrInt16:
            *(epicsInt16 *)pValue = this->value.i16;
            break;
        case NDAttrUInt16:
            *(epicsUInt16 *)pValue = this->value.ui16;
            break;
        case NDAttrInt32:
            *(epicsInt32*)pValue = this->value.i32;
            break;
        case NDAttrUInt32:
            *(epicsUInt32 *)pValue = this->value.ui32;
            break;
        case NDAttrFloat32:
            *(epicsFloat32 *)pValue = this->value.f32;
            break;
        case NDAttrFloat64:
            *(epicsFloat64 *)pValue = this->value.f64;
            break;
        case NDAttrString:
            if (!this->pString) return (ND_ERROR);
            if (dataSize == 0) dataSize = strlen(this->pString)+1;
            strncpy((char *)pValue, this->pString, dataSize);
            break;
        case NDAttrUndefined:
        default:
            return(ND_ERROR);
            break;
    }
    return(ND_SUCCESS);
}

/** Reports on the properties of the attribute.
  */
int NDAttribute::report(int details)
{
    
    printf("NDAttribute, address=%p:\n", this);
    printf("  name=%s\n", this->pName);
    printf("  description=%s\n", this->pDescription);
    switch (this->dataType) {
        case NDAttrInt8:
            printf("  dataType=NDAttrInt8, value=%d\n", this->value.i8);
            break;
        case NDAttrUInt8:
            printf("  dataType=NDAttrUInt8, value=%u\n", this->value.ui8);
            break;
        case NDAttrInt16:
            printf("  dataType=NDAttrInt16, value=%d\n", this->value.i16);
            break;
        case NDAttrUInt16:
            printf("  dataType=NDAttrUInt16, value=%d\n", this->value.ui16);
            break;
        case NDAttrInt32:
            printf("  dataType=NDAttrInt32, value=%d\n", this->value.i32);
            break;
        case NDAttrUInt32:
            printf("  dataType=NDAttrUInt32, value=%d\n", this->value.ui32);
            break;
        case NDAttrFloat32:
            printf("  dataType=NDAttrFloat32, value=%f\n", this->value.f32);
            break;
        case NDAttrFloat64:
            printf("  dataType=NDAttrFloat64, value=%f\n", this->value.f64);
            break;
        case NDAttrString:
            printf("  dataType=NDAttrString, value=%s\n", this->pString);
            break;
        case NDAttrUndefined:
            printf("  dataType=NDAttrUndefined\n");
            break;
        default:
            printf("  dataType=UNKNOWN\n");
            return(ND_ERROR);
            break;
    }
    return ND_SUCCESS;
}


