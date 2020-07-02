#pragma once

class Publishable;

void log(const String &);
void addLogger(Publishable*);

String getResetReasons();
String timeAgo(int seconds);

float mapfloat(long x, long in_min, long in_max, long out_min, long out_max);

extern const char* adafruitRootCert;

String str(const char *fmtStr, ...);
String str(const std::string &s);
String str(bool v);


template<typename T, uint16_t Size>
class CircularArray {
	T buf_[Size];
	T *head_, *tail_;
	uint16_t count_;
public:
	CircularArray() : head_(buf_), tail_(buf_), count_(0) { }
	~CircularArray() { }

	bool push_front(T v) {
		if (head_ == buf_) head_ = buf_ + Size;
		*--head_ = v;
		if (count_ == Size) {
			if (tail_-- == buf_) tail_ = buf_ + Size - 1;
			return false;
		} else {
			if (count_++ == 0) tail_ = head_;
			return true;
		}
	}

	bool push_back(T v) {
		if (++tail_ == buf_ + Size) tail_ = buf_;
		*tail_ = v;
		if (count_ == Size) {
			if (++head_ == buf_ + Size) head_ = buf_;
			return false;
		} else {
			if (count_++ == 0) head_ = tail_;
			return true;
		}
	}

	T pop_front() {
		T res = *head_++;
		if (head_ == buf_ + Size) head_ = buf_;
		count_--;
		return res;
	}

	T pop_end() {
		T res = *tail_--;
		if (tail_ == buf_) tail_ = buf_ + Size - 1;
		count_--;
		return res;
	}

	T& operator [] (uint16_t index) { return *(buf_ + ((head_ - buf_ + index) % Size)); }
	T& front() { return *head_; } //TODO: add bounds checks
	T& back() { return *tail_; }

	uint16_t inline size() const { return count_; }
	uint16_t inline available() const { return Size - count_; }
	bool inline empty() const { return count_ == 0; }
	bool inline isFull() const { return count_ == Size; }

	void inline clear() {
		head_ = tail_ = buf_;
		count_ = 0;
	}
};
