// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_reader.h"
#include "db/log_writer.h"
#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/random.h"
#include "util/testharness.h"

namespace leveldb {
namespace log {

// 静态函数

// Construct a string of the specified length made out of the supplied
// partial string.
static std::string BigString(const std::string& partial_string, size_t n) {
  std::string result;
  while (result.size() < n) {
    result.append(partial_string);    // 重复添加
  }
  result.resize(n);                   // 重新设置大小
  return result;
}

// Construct a string from a number
static std::string NumberString(int n) {
  char buf[50];
  snprintf(buf, sizeof(buf), "%d.", n);   // 将数字转为string
  return std::string(buf);
}

// Return a skewed potentially long string
static std::string RandomSkewedString(int i, Random* rnd) {     // Random 关键在于这个函数的Skewed，能返回什么，应该是个size_t型数据
  return BigString(NumberString(i), rnd->Skewed(17));
}


// LogTest 测试类

class LogTest {
 private:

  // 继承于写文件的字符串目的
  class StringDest : public WritableFile {
   public:
    std::string contents_;
    // 申命为虚函数，可以被后续的继承
    virtual Status Close() { return Status::OK(); }   
    virtual Status Flush() { return Status::OK(); }
    virtual Status Sync() { return Status::OK(); }
    virtual Status Append(const Slice& slice) {         // slice 是个什么东西？ 切片，存储char*和size
      contents_.append(slice.data(), slice.size());
      return Status::OK();
    }
  };

  // 继承于时序文件的字符串源
  class StringSource : public SequentialFile {
   public:
    Slice contents_;          // 
    bool force_error_;
    bool returned_partial_;
    // 构造函数
    StringSource() : force_error_(false), returned_partial_(false) { }

    // Read 函数
    virtual Status Read(size_t n, Slice* result, char* scratch) {
      ASSERT_TRUE(!returned_partial_) << "must not Read() after eof/error";     // 该方式使用很有意思

      if (force_error_) {         // 基本无法进入
        force_error_ = false;
        returned_partial_ = true;
        return Status::Corruption("read error");    // Corruption 返回功能提示
      }

      if (contents_.size() < n) {
        n = contents_.size();
        returned_partial_ = true;
      }
      *result = Slice(contents_.data(), n);
      contents_.remove_prefix(n);
      return Status::OK();
    }

    // Skip函数
    virtual Status Skip(uint64_t n) {
      if (n > contents_.size()) {
        contents_.clear();
        return Status::NotFound("in-memory file skipped past end");
      }

      contents_.remove_prefix(n);

      return Status::OK();
    }
  };


// 消息收集者，继承于消息
  class ReportCollector : public Reader::Reporter {
   public:
    size_t dropped_bytes_;
    std::string message_;

    ReportCollector() : dropped_bytes_(0) { }               // 简短的函数可直接实现
    virtual void Corruption(size_t bytes, const Status& status) {
      dropped_bytes_ += bytes;
      message_.append(status.ToString());
    }
  };

  // 上述定义的
  StringDest dest_;
  StringSource source_;
  ReportCollector report_;
  bool reading_;

  // 读写
  Writer* writer_;    // 和dest_绑定
  Reader* reader_;    // 和source_， report_绑定

  // Record metadata for testing initial offset functionality
  static size_t initial_offset_record_sizes_[];
  static uint64_t initial_offset_last_record_offsets_[];
  static int num_initial_offset_records_;

 public:
 // 构造函数， new出来新的类，这样可以直接构造
  LogTest() : reading_(false),
              writer_(new Writer(&dest_)),
              reader_(new Reader(&source_, &report_, true/*checksum*/,
                      0/*initial_offset*/)) {
  }

  // 析构函数，主要用于调delete等函数
  ~LogTest() {
    delete writer_;
    delete reader_;
  }

  // 重新打开，先关闭，再打开
  void ReopenForAppend() {
    delete writer_;
    writer_ = new Writer(&dest_, dest_.contents_.size());
  }

  // 写数据，调用基类的实现
  void Write(const std::string& msg) {
    ASSERT_TRUE(!reading_) << "Write() after starting to read";
    writer_->AddRecord(Slice(msg));
  }

  // 需要被写的大小
  size_t WrittenBytes() const {
    return dest_.contents_.size();
  }

  // 调用Reader中的ReadRecord， 此部分需要深入了解
  std::string Read() {
    if (!reading_) {      // reading_ == false
      reading_ = true;
      source_.contents_ = Slice(dest_.contents_);
    }

    std::string scratch;
    Slice record;

    if (reader_->ReadRecord(&record, &scratch)) {
      return record.ToString();
    } else {
      return "EOF";
    }
  }

  // 修改对应位数据
  void IncrementByte(int offset, int delta) {
    dest_.contents_[offset] += delta;
  }

  // 修改对应位数据
  void SetByte(int offset, char new_byte) {
    dest_.contents_[offset] = new_byte;
  }

  // 减少数据长度
  void ShrinkSize(int bytes) {
    dest_.contents_.resize(dest_.contents_.size() - bytes);
  }

  void FixChecksum(int header_offset, int len) {
    // Compute crc of type/len/data
    uint32_t crc = crc32c::Value(&dest_.contents_[header_offset+6], 1 + len);
    crc = crc32c::Mask(crc);
    EncodeFixed32(&dest_.contents_[header_offset], crc);
  }

  void ForceError() {
    source_.force_error_ = true;
  }

  size_t DroppedBytes() const {
    return report_.dropped_bytes_;
  }

  // 报告消息
  std::string ReportMessage() const {
    return report_.message_;
  }

  // 匹配错误消息
  // Returns OK iff recorded error message contains "msg"
  std::string MatchError(const std::string& msg) const {
    if (report_.message_.find(msg) == std::string::npos) {
      return report_.message_;
    } else {
      return "OK";
    }
  }

  // 创建数据，并写入
  void WriteInitialOffsetLog() {
    for (int i = 0; i < num_initial_offset_records_; i++) {
      std::string record(initial_offset_record_sizes_[i],
                         static_cast<char>('a' + i));
      Write(record);
    }
  }

  // 重创reader，并进行偏移读操作
  void StartReadingAt(uint64_t initial_offset) {
    delete reader_;
    reader_ = new Reader(&source_, &report_, true/*checksum*/, initial_offset);
  }

  // 测试用例 返回无记录
  void CheckOffsetPastEndReturnsNoRecords(uint64_t offset_past_end) {
    WriteInitialOffsetLog();
    reading_ = true;
    source_.contents_ = Slice(dest_.contents_);
    Reader* offset_reader = new Reader(&source_, &report_, true/*checksum*/,
                                       WrittenBytes() + offset_past_end);
    Slice record;
    std::string scratch;
    ASSERT_TRUE(!offset_reader->ReadRecord(&record, &scratch));
    delete offset_reader;
  }

  // 测试用例
  void CheckInitialOffsetRecord(uint64_t initial_offset,
                                int expected_record_offset) {
    WriteInitialOffsetLog();
    reading_ = true;
    source_.contents_ = Slice(dest_.contents_);
    Reader* offset_reader = new Reader(&source_, &report_, true/*checksum*/,
                                       initial_offset);

    // Read all records from expected_record_offset through the last one.
    ASSERT_LT(expected_record_offset, num_initial_offset_records_);
    for (; expected_record_offset < num_initial_offset_records_;
         ++expected_record_offset) {
      Slice record;
      std::string scratch;
      ASSERT_TRUE(offset_reader->ReadRecord(&record, &scratch));
      ASSERT_EQ(initial_offset_record_sizes_[expected_record_offset],
                record.size());
      ASSERT_EQ(initial_offset_last_record_offsets_[expected_record_offset],
                offset_reader->LastRecordOffset());
      ASSERT_EQ((char)('a' + expected_record_offset), record.data()[0]);
    }
    delete offset_reader;
  }
};



// 测试参数
size_t LogTest::initial_offset_record_sizes_[] =
    {10000,  // Two sizable records in first block
     10000,
     2 * log::kBlockSize - 1000,  // Span three blocks
     1,
     13716,  // Consume all but two bytes of block 3.
     log::kBlockSize - kHeaderSize, // Consume the entirety of block 4.
    };

uint64_t LogTest::initial_offset_last_record_offsets_[] =
    {0,
     kHeaderSize + 10000,
     2 * (kHeaderSize + 10000),
     2 * (kHeaderSize + 10000) +
         (2 * log::kBlockSize - 1000) + 3 * kHeaderSize,
     2 * (kHeaderSize + 10000) +
         (2 * log::kBlockSize - 1000) + 3 * kHeaderSize
         + kHeaderSize + 1,
     3 * log::kBlockSize,
    };

// LogTest::initial_offset_last_record_offsets_ must be defined before this.
int LogTest::num_initial_offset_records_ =
    sizeof(LogTest::initial_offset_last_record_offsets_)/sizeof(uint64_t);



// 单元测试样例，输入输出的比对
TEST(LogTest, Empty) {
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, ReadWrite) {
  Write("foo");
  Write("bar");
  Write("");
  Write("xxxx");
  ASSERT_EQ("foo", Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("", Read());
  ASSERT_EQ("xxxx", Read());
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ("EOF", Read());  // Make sure reads at eof work
}

TEST(LogTest, ManyBlocks) {
  for (int i = 0; i < 100000; i++) {
    Write(NumberString(i));
  }
  for (int i = 0; i < 100000; i++) {
    ASSERT_EQ(NumberString(i), Read());
  }
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, Fragmentation) {
  Write("small");
  Write(BigString("medium", 50000));
  Write(BigString("large", 100000));
  ASSERT_EQ("small", Read());
  ASSERT_EQ(BigString("medium", 50000), Read());
  ASSERT_EQ(BigString("large", 100000), Read());
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, MarginalTrailer) {
  // Make a trailer that is exactly the same length as an empty record.
  const int n = kBlockSize - 2*kHeaderSize;
  Write(BigString("foo", n));
  ASSERT_EQ(kBlockSize - kHeaderSize, WrittenBytes());
  Write("");
  Write("bar");
  ASSERT_EQ(BigString("foo", n), Read());
  ASSERT_EQ("", Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, MarginalTrailer2) {
  // Make a trailer that is exactly the same length as an empty record.
  const int n = kBlockSize - 2*kHeaderSize;
  Write(BigString("foo", n));
  ASSERT_EQ(kBlockSize - kHeaderSize, WrittenBytes());
  Write("bar");
  ASSERT_EQ(BigString("foo", n), Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(0, DroppedBytes());
  ASSERT_EQ("", ReportMessage());
}

TEST(LogTest, ShortTrailer) {
  const int n = kBlockSize - 2*kHeaderSize + 4;
  Write(BigString("foo", n));
  ASSERT_EQ(kBlockSize - kHeaderSize + 4, WrittenBytes());
  Write("");
  Write("bar");
  ASSERT_EQ(BigString("foo", n), Read());
  ASSERT_EQ("", Read());
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, AlignedEof) {
  const int n = kBlockSize - 2*kHeaderSize + 4;
  Write(BigString("foo", n));
  ASSERT_EQ(kBlockSize - kHeaderSize + 4, WrittenBytes());
  ASSERT_EQ(BigString("foo", n), Read());
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, OpenForAppend) {
  Write("hello");
  ReopenForAppend();
  Write("world");
  ASSERT_EQ("hello", Read());
  ASSERT_EQ("world", Read());
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, RandomRead) {
  const int N = 500;
  Random write_rnd(301);
  for (int i = 0; i < N; i++) {
    Write(RandomSkewedString(i, &write_rnd));
  }
  Random read_rnd(301);
  for (int i = 0; i < N; i++) {
    ASSERT_EQ(RandomSkewedString(i, &read_rnd), Read());
  }
  ASSERT_EQ("EOF", Read());
}

// Tests of all the error paths in log_reader.cc follow:

TEST(LogTest, ReadError) {
  Write("foo");
  ForceError();
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(kBlockSize, DroppedBytes());
  ASSERT_EQ("OK", MatchError("read error"));
}

TEST(LogTest, BadRecordType) {
  Write("foo");
  // Type is stored in header[6]
  IncrementByte(6, 100);
  FixChecksum(0, 3);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(3, DroppedBytes());
  ASSERT_EQ("OK", MatchError("unknown record type"));
}

TEST(LogTest, TruncatedTrailingRecordIsIgnored) {
  Write("foo");
  ShrinkSize(4);   // Drop all payload as well as a header byte
  ASSERT_EQ("EOF", Read());
  // Truncated last record is ignored, not treated as an error.
  ASSERT_EQ(0, DroppedBytes());
  ASSERT_EQ("", ReportMessage());
}

TEST(LogTest, BadLength) {
  const int kPayloadSize = kBlockSize - kHeaderSize;
  Write(BigString("bar", kPayloadSize));
  Write("foo");
  // Least significant size byte is stored in header[4].
  IncrementByte(4, 1);
  ASSERT_EQ("foo", Read());
  ASSERT_EQ(kBlockSize, DroppedBytes());
  ASSERT_EQ("OK", MatchError("bad record length"));
}

TEST(LogTest, BadLengthAtEndIsIgnored) {
  Write("foo");
  ShrinkSize(1);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(0, DroppedBytes());
  ASSERT_EQ("", ReportMessage());
}

TEST(LogTest, ChecksumMismatch) {
  Write("foo");
  IncrementByte(0, 10);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(10, DroppedBytes());
  ASSERT_EQ("OK", MatchError("checksum mismatch"));
}

TEST(LogTest, UnexpectedMiddleType) {
  Write("foo");
  SetByte(6, kMiddleType);
  FixChecksum(0, 3);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(3, DroppedBytes());
  ASSERT_EQ("OK", MatchError("missing start"));
}

TEST(LogTest, UnexpectedLastType) {
  Write("foo");
  SetByte(6, kLastType);
  FixChecksum(0, 3);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(3, DroppedBytes());
  ASSERT_EQ("OK", MatchError("missing start"));
}

TEST(LogTest, UnexpectedFullType) {
  Write("foo");
  Write("bar");
  SetByte(6, kFirstType);
  FixChecksum(0, 3);
  ASSERT_EQ("bar", Read());
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(3, DroppedBytes());
  ASSERT_EQ("OK", MatchError("partial record without end"));
}

TEST(LogTest, UnexpectedFirstType) {
  Write("foo");
  Write(BigString("bar", 100000));
  SetByte(6, kFirstType);
  FixChecksum(0, 3);
  ASSERT_EQ(BigString("bar", 100000), Read());
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ(3, DroppedBytes());
  ASSERT_EQ("OK", MatchError("partial record without end"));
}

TEST(LogTest, MissingLastIsIgnored) {
  Write(BigString("bar", kBlockSize));
  // Remove the LAST block, including header.
  ShrinkSize(14);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ("", ReportMessage());
  ASSERT_EQ(0, DroppedBytes());
}

TEST(LogTest, PartialLastIsIgnored) {
  Write(BigString("bar", kBlockSize));
  // Cause a bad record length in the LAST block.
  ShrinkSize(1);
  ASSERT_EQ("EOF", Read());
  ASSERT_EQ("", ReportMessage());
  ASSERT_EQ(0, DroppedBytes());
}

TEST(LogTest, SkipIntoMultiRecord) {
  // Consider a fragmented record:
  //    first(R1), middle(R1), last(R1), first(R2)
  // If initial_offset points to a record after first(R1) but before first(R2)
  // incomplete fragment errors are not actual errors, and must be suppressed
  // until a new first or full record is encountered.
  Write(BigString("foo", 3*kBlockSize));
  Write("correct");
  StartReadingAt(kBlockSize);

  ASSERT_EQ("correct", Read());
  ASSERT_EQ("", ReportMessage());
  ASSERT_EQ(0, DroppedBytes());
  ASSERT_EQ("EOF", Read());
}

TEST(LogTest, ErrorJoinsRecords) {
  // Consider two fragmented records:
  //    first(R1) last(R1) first(R2) last(R2)
  // where the middle two fragments disappear.  We do not want
  // first(R1),last(R2) to get joined and returned as a valid record.

  // Write records that span two blocks
  Write(BigString("foo", kBlockSize));
  Write(BigString("bar", kBlockSize));
  Write("correct");

  // Wipe the middle block
  for (int offset = kBlockSize; offset < 2*kBlockSize; offset++) {
    SetByte(offset, 'x');
  }

  ASSERT_EQ("correct", Read());
  ASSERT_EQ("EOF", Read());
  const size_t dropped = DroppedBytes();
  ASSERT_LE(dropped, 2*kBlockSize + 100);
  ASSERT_GE(dropped, 2*kBlockSize);
}

TEST(LogTest, ReadStart) {
  CheckInitialOffsetRecord(0, 0);
}

TEST(LogTest, ReadSecondOneOff) {
  CheckInitialOffsetRecord(1, 1);
}

TEST(LogTest, ReadSecondTenThousand) {
  CheckInitialOffsetRecord(10000, 1);
}

TEST(LogTest, ReadSecondStart) {
  CheckInitialOffsetRecord(10007, 1);
}

TEST(LogTest, ReadThirdOneOff) {
  CheckInitialOffsetRecord(10008, 2);
}

TEST(LogTest, ReadThirdStart) {
  CheckInitialOffsetRecord(20014, 2);
}

TEST(LogTest, ReadFourthOneOff) {
  CheckInitialOffsetRecord(20015, 3);
}

TEST(LogTest, ReadFourthFirstBlockTrailer) {
  CheckInitialOffsetRecord(log::kBlockSize - 4, 3);
}

TEST(LogTest, ReadFourthMiddleBlock) {
  CheckInitialOffsetRecord(log::kBlockSize + 1, 3);
}

TEST(LogTest, ReadFourthLastBlock) {
  CheckInitialOffsetRecord(2 * log::kBlockSize + 1, 3);
}

TEST(LogTest, ReadFourthStart) {
  CheckInitialOffsetRecord(
      2 * (kHeaderSize + 1000) + (2 * log::kBlockSize - 1000) + 3 * kHeaderSize,
      3);
}

TEST(LogTest, ReadInitialOffsetIntoBlockPadding) {
  CheckInitialOffsetRecord(3 * log::kBlockSize - 3, 5);
}

TEST(LogTest, ReadEnd) {
  CheckOffsetPastEndReturnsNoRecords(0);
}

TEST(LogTest, ReadPastEnd) {
  CheckOffsetPastEndReturnsNoRecords(5);
}

}  // namespace log
}  // namespace leveldb

int main(int argc, char** argv) {
  return leveldb::test::RunAllTests();
}
