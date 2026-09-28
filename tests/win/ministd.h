/* ministd.h - the shapes a C++ program actually uses.
 *
 * There is no Microsoft standard library here, and the point of this file is
 * not to be one.  It is to find out whether the runtime underneath can carry
 * one: containers that own memory and give it back, a string that grows, a
 * pointer that cleans up after itself, and all of it correct when something
 * throws part way through.
 *
 * Every one of those leans on something that had to be built for it - memory
 * through operators, constructors and destructors running at the right
 * moments, objects taken apart in reverse order when a throw goes past.  A
 * runtime that gets any of those wrong fails here rather than in something
 * subtle later.
 */
#ifndef KESTREL_MINISTD_H
#define KESTREL_MINISTD_H

#include "winapi.h"

namespace mini {

/* ---------------------------------------------------------------- moving */

template <typename T> struct remove_reference      { typedef T type; };
template <typename T> struct remove_reference<T&>  { typedef T type; };
template <typename T> struct remove_reference<T&&> { typedef T type; };

template <typename T>
typename remove_reference<T>::type &&move(T &&x) {
    return static_cast<typename remove_reference<T>::type &&>(x);
}

/* -------------------------------------------------- a pointer that tidies up */

/* Owning something and giving it back when it goes out of scope, including
 * when it goes out of scope because something threw. */
template <typename T>
class unique_ptr {
public:
    unique_ptr() : held(nullptr) {}
    explicit unique_ptr(T *p) : held(p) {}
    unique_ptr(unique_ptr &&other) : held(other.held) { other.held = nullptr; }
    ~unique_ptr() { delete held; }

    unique_ptr &operator=(unique_ptr &&other) {
        if (this != &other) {
            delete held;
            held = other.held;
            other.held = nullptr;
        }
        return *this;
    }

    T *get() const { return held; }
    T &operator*() const { return *held; }
    T *operator->() const { return held; }
    explicit operator bool() const { return held != nullptr; }

    T *release() { T *p = held; held = nullptr; return p; }
    void reset(T *p = nullptr) { delete held; held = p; }

private:
    unique_ptr(const unique_ptr &);
    unique_ptr &operator=(const unique_ptr &);
    T *held;
};

/* ------------------------------------------------------- a growing sequence */

template <typename T>
class vector {
public:
    vector() : items(nullptr), used(0), room(0) {}
    vector(const vector &other) : items(nullptr), used(0), room(0) {
        reserve(other.used);
        for (unsigned i = 0; i < other.used; i++) push_back(other.items[i]);
    }
    ~vector() { clear(); ::operator delete(items); }

    vector &operator=(const vector &other) {
        if (this != &other) {
            clear();
            reserve(other.used);
            for (unsigned i = 0; i < other.used; i++) push_back(other.items[i]);
        }
        return *this;
    }

    void push_back(const T &value) {
        if (used == room) reserve(room ? room * 2 : 4);
        /* Built where it will live rather than assigned into place, because
         * the space is raw and there is nothing there to assign over. */
        new (static_cast<void *>(&items[used])) T(value);
        used++;
    }

    void pop_back() { if (used) { used--; items[used].~T(); } }

    void clear() {
        /* In reverse, which is the order things are taken apart in. */
        while (used) pop_back();
    }

    void reserve(unsigned want) {
        if (want <= room) return;
        T *bigger = static_cast<T *>(::operator new(want * sizeof(T)));
        for (unsigned i = 0; i < used; i++) {
            new (static_cast<void *>(&bigger[i])) T(items[i]);
            items[i].~T();
        }
        ::operator delete(items);
        items = bigger;
        room = want;
    }

    unsigned size() const { return used; }
    bool empty() const { return used == 0; }
    T &operator[](unsigned i) { return items[i]; }
    const T &operator[](unsigned i) const { return items[i]; }
    T *begin() { return items; }
    T *end() { return items + used; }

private:
    T *items;
    unsigned used, room;
};

/* --------------------------------------------------------- a growing string */

class string {
public:
    string() : text(nullptr), length(0), room(0) { assign(""); }
    string(const char *s) : text(nullptr), length(0), room(0) { assign(s); }
    string(const string &other) : text(nullptr), length(0), room(0) {
        assign(other.text);
    }
    ~string() { ::operator delete(text); }

    string &operator=(const string &other) {
        if (this != &other) assign(other.text);
        return *this;
    }

    string &operator+=(const char *s) {
        unsigned add = measure(s);
        grow(length + add);
        for (unsigned i = 0; i <= add; i++) text[length + i] = s[i];
        length += add;
        return *this;
    }
    string &operator+=(const string &s) { return *this += s.text; }

    string operator+(const string &s) const {
        string out(*this);
        out += s;
        return out;
    }

    bool operator==(const string &s) const {
        if (length != s.length) return false;
        for (unsigned i = 0; i < length; i++)
            if (text[i] != s.text[i]) return false;
        return true;
    }

    unsigned size() const { return length; }
    const char *c_str() const { return text; }
    char &operator[](unsigned i) { return text[i]; }

private:
    static unsigned measure(const char *s) {
        unsigned n = 0;
        while (s && s[n]) n++;
        return n;
    }

    void grow(unsigned want) {
        if (want + 1 <= room) return;
        unsigned bigger = room ? room * 2 : 16;
        while (bigger < want + 1) bigger *= 2;
        char *fresh = static_cast<char *>(::operator new(bigger));
        for (unsigned i = 0; i <= length; i++) fresh[i] = text ? text[i] : 0;
        ::operator delete(text);
        text = fresh;
        room = bigger;
    }

    void assign(const char *s) {
        unsigned n = measure(s);
        length = 0;
        grow(n);
        for (unsigned i = 0; i <= n; i++) text[i] = s ? s[i] : 0;
        length = n;
    }

    char *text;
    unsigned length, room;
};

}  /* namespace mini */

/* Building something where it already sits, which a container that owns raw
 * space cannot do without. */
inline void *operator new(SIZE_T, void *where) { return where; }
inline void operator delete(void *, void *) {}

#endif
