#pragma once
#include "Exec.hpp"

#include <algorithm>
#include <vector>

namespace Kvasir { namespace Register {

    struct ReadValue {
        unsigned address_;
        unsigned value_;
    };

    struct RecordedAction {
        enum class Type { unknown, read, write, writeLiteral };
        Type     type_;
        unsigned address_;
        unsigned mask_;
        unsigned value_;
    };

    class Reads {
        std::vector<ReadValue> data_;

    public:
        using iterator = typename std::vector<ReadValue>::iterator;

        ReadValue& operator[](size_t i) { return data_[i]; }

        iterator begin() { return data_.begin(); }

        iterator end() { return data_.end(); }

        void push(ReadValue const& val) {
            data_.insert(std::lower_bound(data_.begin(),
                                          data_.end(),
                                          val,
                                          [](ReadValue const& lhs, ReadValue const& rhs) {
                                              return lhs.address_ < rhs.address_;
                                          }),
                         val);
        }

        ReadValue pop(unsigned address) {
            ReadValue ret{0, 0};
            auto      it = std::lower_bound(
              data_.begin(),
              data_.end(),
              address,
              [](ReadValue const& lhs, unsigned const rhs) { return lhs.address_ < rhs; });
            if(it != data_.end() && it->address_ == address) {
                ret = *it;
                data_.erase(it);
            }
            return ret;
        }
    };

    std::vector<RecordedAction> actions_;
    Reads                       reads_;

    template<typename T>
    struct RecordActions {
        int operator()(int in) {
            actions_.emplace_back(RecordedAction{RecordedAction::Type::unknown});
            return 0;
        }
    };

    template<typename Address, unsigned Mask, typename Access, typename FieldType>
    struct RecordActions<Action<FieldLocation<Address, Mask, Access, FieldType>, ReadAction>> {
        int operator()(int in) {
            actions_.emplace_back(RecordedAction{RecordedAction::Type::read, Address::value, Mask});
            auto it = std::find_if(reads_.begin(), reads_.end(), [](ReadValue& v) {
                return v.address_ == Address::value;
            });
            if(it != reads_.end()) { return reads_.pop(Address::value).value_; }
            return 0;
        }
    };

    template<typename Address, unsigned Mask, typename Access, typename FieldType>
    struct RecordActions<Action<FieldLocation<Address, Mask, Access, FieldType>, WriteAction>> {
        int operator()(unsigned in) {
            actions_.emplace_back(
              RecordedAction{RecordedAction::Type::write, Address::value, Mask, in});
            return 0;
        }
    };

    template<typename Address, unsigned Mask, typename Access, typename FieldType, unsigned I>
    struct RecordActions<
      Action<FieldLocation<Address, Mask, Access, FieldType>, WriteLiteralAction<I>>> {
        int operator()(int in) {
            actions_.emplace_back(
              RecordedAction{RecordedAction::Type::writeLiteral, Address::value, Mask, I});
            return 0;
        }
    };
}}   // namespace Kvasir::Register
