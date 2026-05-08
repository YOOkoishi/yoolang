#pragma once

#include "../ast/ast.h"
#include "../mir/MIR.h"
#include "../oir/oir.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pass {

enum class PassKind {
    Analysis,
    Transform,
    Lowering,
    Verification,
};

std::string_view to_string(PassKind kind);

struct PassResult {
    bool success = true;
    bool changed = false;
    std::string message;

    static PassResult ok(bool changed = false, std::string message = "");
    static PassResult fail(std::string message);
};

class PassContext {
  private:
    struct AnalysisBase;
    template <typename T> struct AnalysisModel;

  public:
    PassContext() = default;

    bool has_ast() const;
    CompUnit *ast();
    const CompUnit *ast() const;
    void set_ast(std::unique_ptr<CompUnit> ast);
    std::unique_ptr<CompUnit> take_ast();

    bool has_ssa_module() const;
    oir::Module *ssa_module();
    const oir::Module *ssa_module() const;
    void set_ssa_module(std::unique_ptr<oir::Module> module);
    std::unique_ptr<oir::Module> take_ssa_module();

    bool has_machine_module() const;
    mir::Module *machine_module();
    const mir::Module *machine_module() const;
    void set_machine_module(std::unique_ptr<mir::Module> module);
    std::unique_ptr<mir::Module> take_machine_module();

    bool has_artifact(const std::string &key) const;
    void erase_artifact(const std::string &key);

    template <typename T> void set_artifact(const std::string &key, T value) {
        artifacts_[key] = std::make_unique<Artifact<T>>(std::move(value));
    }

    template <typename T> void set_artifact(T value) {
        set_artifact(default_artifact_key<T>(), std::move(value));
    }

    template <typename T> T *get_artifact(const std::string &key) {
        auto it = artifacts_.find(key);
        if (it == artifacts_.end()) {
            return nullptr;
        }
        auto *artifact = dynamic_cast<Artifact<T> *>(it->second.get());
        return artifact == nullptr ? nullptr : &artifact->value;
    }

    template <typename T> T *get_artifact() {
        return get_artifact<T>(default_artifact_key<T>());
    }

    template <typename T> const T *get_artifact(const std::string &key) const {
        auto it = artifacts_.find(key);
        if (it == artifacts_.end()) {
            return nullptr;
        }
        auto *artifact = dynamic_cast<const Artifact<T> *>(it->second.get());
        return artifact == nullptr ? nullptr : &artifact->value;
    }

    template <typename T> const T *get_artifact() const {
        return get_artifact<T>(default_artifact_key<T>());
    }

    template <typename T> T take_artifact(const std::string &key) {
        auto it = artifacts_.find(key);
        if (it == artifacts_.end()) {
            throw std::runtime_error("artifact not found: " + key);
        }
        auto *artifact = dynamic_cast<Artifact<T> *>(it->second.get());
        if (artifact == nullptr) {
            throw std::runtime_error("artifact has unexpected type: " + key);
        }
        T value = std::move(artifact->value);
        artifacts_.erase(it);
        return value;
    }

    template <typename AnalysisT> AnalysisT *get_analysis() {
        auto it = oir_analyses_.find(std::type_index(typeid(AnalysisT)));
        if (it == oir_analyses_.end()) {
            return nullptr;
        }
        auto *model = dynamic_cast<AnalysisModel<AnalysisT> *>(it->second.get());
        return model == nullptr ? nullptr : &model->value;
    }

    template <typename AnalysisT> const AnalysisT *get_analysis() const {
        auto it = oir_analyses_.find(std::type_index(typeid(AnalysisT)));
        if (it == oir_analyses_.end()) {
            return nullptr;
        }
        auto *model = dynamic_cast<const AnalysisModel<AnalysisT> *>(it->second.get());
        return model == nullptr ? nullptr : &model->value;
    }

    template <typename AnalysisT, typename... Args>
    AnalysisT &get_or_build_analysis(Args &&...args) {
        auto key = std::type_index(typeid(AnalysisT));
        auto found = oir_analyses_.find(key);
        if (found != oir_analyses_.end()) {
            auto *model = dynamic_cast<AnalysisModel<AnalysisT> *>(found->second.get());
            if (model == nullptr) {
                throw std::runtime_error("cached OIR analysis has unexpected type");
            }
            return model->value;
        }

        auto analysis = std::make_unique<AnalysisModel<AnalysisT>>(std::forward<Args>(args)...);
        auto *raw = analysis.get();
        oir_analyses_[key] = std::move(analysis);
        return raw->value;
    }

    void invalidate_oir_analyses();

  private:
    struct ArtifactBase {
        virtual ~ArtifactBase() = default;
    };

    template <typename T> struct Artifact final : ArtifactBase {
        explicit Artifact(T value) : value(std::move(value)) {
        }
        T value;
    };

    struct AnalysisBase {
        virtual ~AnalysisBase() = default;
    };

    template <typename T> struct AnalysisModel final : AnalysisBase {
        template <typename... Args>
        explicit AnalysisModel(Args &&...args) : value(std::forward<Args>(args)...) {
        }
        T value;
    };

    template <typename T> static std::string default_artifact_key() {
        return typeid(T).name();
    }

    std::unique_ptr<CompUnit> ast_;
    std::unique_ptr<oir::Module> ssa_module_;
    std::unique_ptr<mir::Module> machine_module_;
    std::unordered_map<std::string, std::unique_ptr<ArtifactBase>> artifacts_;
    std::unordered_map<std::type_index, std::unique_ptr<AnalysisBase>> oir_analyses_;
};

class Pass {
  public:
    virtual ~Pass() = default;

    virtual std::string_view name() const = 0;
    virtual PassKind kind() const = 0;
    virtual PassResult run(PassContext &context) = 0;
};

struct PassExecution {
    std::string name;
    PassKind kind = PassKind::Transform;
    PassResult result;
};

struct PassManagerResult {
    bool success = true;
    bool changed = false;
    std::vector<PassExecution> executions;
};

class PassManager {
  public:
    PassManager() = default;

    void set_stop_on_failure(bool stop_on_failure);
    bool stop_on_failure() const;

    void register_pass(std::unique_ptr<Pass> pass);

    template <typename PassT, typename... Args> PassT &emplace_pass(Args &&...args) {
        auto pass = std::make_unique<PassT>(std::forward<Args>(args)...);
        auto *raw = pass.get();
        register_pass(std::move(pass));
        return *raw;
    }

    void clear();
    std::size_t size() const;
    bool empty() const;

    PassManagerResult run(PassContext &context);

  private:
    bool stop_on_failure_ = true;
    std::vector<std::unique_ptr<Pass>> passes_;
};

} // namespace pass
