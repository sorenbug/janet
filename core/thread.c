/*
* Copyright (c) 2017 Calvin Rose
* 
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
* 
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include <gst/gst.h>

/* Create a new thread */
GstThread *gst_thread(Gst *vm, GstValue callee, uint32_t capacity) {
    GstThread *thread = gst_alloc(vm, sizeof(GstThread));
    GstValue *data, *stack;
    if (capacity < GST_FRAME_SIZE) capacity = GST_FRAME_SIZE;
    data = gst_alloc(vm, sizeof(GstValue) * capacity);
    thread->capacity = capacity;
    thread->count = GST_FRAME_SIZE;
    thread->data = data;
    thread->status = GST_THREAD_PENDING;
    stack = data + GST_FRAME_SIZE;
    gst_frame_size(stack) = 0;
    gst_frame_prevsize(stack) = 0;
    gst_frame_ret(stack) = 0;
    gst_frame_args(stack) = 0;
    gst_frame_pc(stack) = NULL;
    gst_frame_env(stack) = NULL;
    gst_frame_callee(stack) = callee;
    gst_thread_endframe(vm, thread);
    thread->parent = vm->thread;
    return thread;
}

/* Ensure that the thread has enough EXTRA capacity */
void gst_thread_ensure_extra(Gst *vm, GstThread *thread, uint32_t extra) {
    GstValue *newData, *stack;
    uint32_t usedCapacity, neededCapacity, newCapacity;
    stack = thread->data + thread->count;
    usedCapacity = thread->count + gst_frame_size(stack) + GST_FRAME_SIZE;
    neededCapacity = usedCapacity + extra;
    if (thread->capacity >= neededCapacity) return;
    newCapacity = 2 * neededCapacity;
    newData = gst_alloc(vm, sizeof(GstValue) * newCapacity);
    gst_memcpy(newData, thread->data, sizeof(GstValue) * usedCapacity);
    thread->data = newData;
    thread->capacity = newCapacity;
}

/* Push a value on the current stack frame*/
void gst_thread_push(Gst *vm, GstThread *thread, GstValue x) {
    GstValue *stack;
    gst_thread_ensure_extra(vm, thread, 1);
    stack = thread->data + thread->count;
    stack[gst_frame_size(stack)++] = x;
}

/* Push n nils onto the stack */
void gst_thread_pushnil(Gst *vm, GstThread *thread, uint32_t n) {
    GstValue *stack, *current, *end;
    gst_thread_ensure_extra(vm, thread, n);
    stack = thread->data + thread->count;
    current = stack + gst_frame_size(stack);
    end = current + n;
    for (; current < end; ++current) {
        current->type = GST_NIL;
    }
    gst_frame_size(stack) += n;
}

/* Package up extra args after and including n into tuple at n*/
void gst_thread_tuplepack(Gst *vm, GstThread *thread, uint32_t n) {
    GstValue *stack = thread->data + thread->count;
    uint32_t size = gst_frame_size(stack);
    if (n > size) {
        /* Push one extra nil to ensure space for tuple */
        gst_thread_pushnil(vm, thread, n - size + 1);
        stack = thread->data + thread->count;
        stack[n].type = GST_TUPLE;
        stack[n].data.tuple = gst_tuple_end(vm, gst_tuple_begin(vm, 0));
        gst_frame_size(stack) = n + 1;
    } else {
        uint32_t i;
        GstValue *tuple = gst_tuple_begin(vm, size - n);
        for (i = n; i < size; ++i)
            tuple[i - n] = stack[i];
        stack[n].type = GST_TUPLE;
        stack[n].data.tuple = gst_tuple_end(vm, tuple);
    }
}

/* Push a stack frame to a thread, with space for arity arguments. Returns the new
 * stack. */
GstValue *gst_thread_beginframe(Gst *vm, GstThread *thread, GstValue callee, uint32_t arity) {
    uint32_t frameOffset;
    GstValue *oldStack, *newStack;

    /* Push the frame */
    gst_thread_ensure_extra(vm, thread, GST_FRAME_SIZE + arity + 4);
    oldStack = thread->data + thread->count;
    frameOffset = gst_frame_size(oldStack) + GST_FRAME_SIZE;
    newStack = oldStack + frameOffset;
    gst_frame_prevsize(newStack) = gst_frame_size(oldStack);
    gst_frame_env(newStack) = NULL;
    gst_frame_size(newStack) = 0;
    gst_frame_callee(newStack) = callee;
    thread->count += frameOffset; 
    
    /* Ensure the extra space and initialize to nil */
    gst_thread_pushnil(vm, thread, arity);

    /* Return ok */
    return thread->data + thread->count;
}

/* After pushing arguments to a stack frame created with gst_thread_beginframe, call this
 * to finalize the frame before starting a function call. */
void gst_thread_endframe(Gst *vm, GstThread *thread) {
    GstValue *stack = thread->data + thread->count;
    GstValue callee = gst_frame_callee(stack);
    if (callee.type == GST_FUNCTION) {
        GstFunction *fn = callee.data.function;
        gst_frame_pc(stack) = fn->def->byteCode;
        if (fn->def->flags & GST_FUNCDEF_FLAG_VARARG) {
            uint32_t arity = fn->def->arity;
            gst_thread_tuplepack(vm, thread, arity);
        } else {
            uint32_t locals = fn->def->locals;
            if (gst_frame_size(stack) < locals) {
                gst_thread_pushnil(vm, thread, locals - gst_frame_size(stack));
            }
        }
        stack = thread->data + thread->count;
        gst_frame_args(stack) = gst_frame_size(stack) + GST_FRAME_SIZE;
    }
}

/* Pop a stack frame from the thread. Returns the new stack frame, or
 * NULL if there are no more frames */
GstValue *gst_thread_popframe(Gst *vm, GstThread *thread) {
    GstValue *stack = thread->data + thread->count;
    uint32_t prevsize = gst_frame_prevsize(stack);
    GstValue *nextstack = stack - GST_FRAME_SIZE - prevsize;
    GstFuncEnv *env = gst_frame_env(stack);

    /* Check for closures */
    if (env != NULL) {
        uint32_t size = gst_frame_size(stack);
        env->thread = NULL;
        env->stackOffset = size;
        env->values = gst_alloc(vm, sizeof(GstValue) * size);
        gst_memcpy(env->values, stack, sizeof(GstValue) * size);
    }

    /* Shrink stack */
    thread->count -= GST_FRAME_SIZE + prevsize;

    /* Check if the stack is empty, and if so, return null */
    if (thread->count)
        return nextstack;
    else
        return NULL;
}

/* Count the number of stack frames in a thread */
uint32_t gst_thread_countframes(GstThread *thread) {
    uint32_t count = 0;
    const GstValue *stack = thread->data + GST_FRAME_SIZE;
    const GstValue *laststack = thread->data + thread->count;
    while (stack <= laststack) {
        ++count;
        stack += gst_frame_size(stack) + GST_FRAME_SIZE;
    }
    return count;
}
