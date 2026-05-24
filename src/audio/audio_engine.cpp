#include "audio/audio_engine.h"
#include "audio/audio_backend.h"
#include "audio/effect_factory.h"
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace Amplitron {

AudioEngine::AudioEngine() {
    process_buffer_.resize(MAX_BUFFER_SIZE, 0.0f);
    process_buffer_right_.resize(MAX_BUFFER_SIZE, 0.0f);
    backend_ = create_audio_backend();

    // Pre-allocate the graph memory pools immediately on startup
    main_executor_ = std::make_shared<AudioGraphExecutor>();
    // Assuming standard values, use your engine's actual sample rate / block size variables here
    main_executor_->prepare(48000, 512, 32); 
    
    // Seed the shadow executor so the audio thread has something safe to read instantly
    audio_shadow_executor_ = main_executor_;
}

AudioEngine::~AudioEngine() {
    shutdown();
    
    destroy_audio_backend(backend_);
    backend_ = nullptr;
}

// --- Serialization Methods ---


nlohmann::json AudioEngine::serialize() {
    std::lock_guard<std::mutex> lock(effect_mutex_);
    nlohmann::json j;
    
    // Read atomic variables safely
    j["input_gain"] = input_gain_.load(std::memory_order_relaxed);
    
    auto nodes_array = nlohmann::json::array();
    for (const auto& node : main_graph_.get_nodes()) {
        nlohmann::json j_node;
        j_node["id"] = node.id;
        j_node["name"] = node.name;
        j_node["routing_type"] = static_cast<int>(node.routing_type);
        j_node["is_input"] = node.is_graph_input;
        j_node["is_output"] = node.is_graph_output;
        j_node["x"] = node.x;
        j_node["y"] = node.y;
        
        j_node["input_pins"] = node.input_pin_ids;
        j_node["output_pins"] = node.output_pin_ids;
        
        if (node.pedal) {
            j_node["effect_type"] = node.pedal->type_id();
            j_node["params"] = node.pedal->get_params();
        }
        nodes_array.push_back(j_node);
    }
    j["nodes"] = nodes_array;
    
    auto links_array = nlohmann::json::array();
    for (const auto& link : main_graph_.get_links()) {
        nlohmann::json j_link;
        j_link["id"] = link.id;
        j_link["source"] = link.source_pin_id;
        j_link["dest"] = link.dest_pin_id;
        links_array.push_back(j_link);
    }
    j["links"] = links_array;

    // Keep dummy_effects_ for legacy preset compatibility
    auto effects_array = nlohmann::json::array();
    for (const auto& fx : dummy_effects_) {
        if (fx) {
            nlohmann::json fx_json;
            fx_json["name"] = fx->name();
            fx_json["params"] = fx->get_params();
            effects_array.push_back(fx_json);
        }
    }
    j["effects"] = effects_array;

    return j;
}

void AudioEngine::deserialize(const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(effect_mutex_);
    
    if (j.contains("input_gain")) {
        set_input_gain(j["input_gain"]);
    }
    
    // Modern Graph Hydration
    if (j.contains("nodes") && j.contains("links")) {
        AudioGraph new_graph;
        std::unordered_map<int, int> pin_map;
        
        for (const auto& j_node : j["nodes"]) {
            std::string name = j_node.value("name", "Unknown");
            NodeRoutingType routing_type = static_cast<NodeRoutingType>(j_node.value("routing_type", 0));
            
            std::shared_ptr<Effect> pedal = nullptr;
            if (j_node.contains("effect_type")) {
                std::string effect_type = j_node["effect_type"];
                pedal = EffectFactory::instance().create(effect_type);
                if (pedal && j_node.contains("params")) {
                    pedal->set_params(j_node["params"]);
                }
            }
            
            int new_node_id = new_graph.add_node(name, routing_type, pedal);
            const DSPNode* new_node = new_graph.find_node(new_node_id);
            if (new_node) {
                new_graph.set_node_as_input(new_node_id, j_node.value("is_input", false));
                new_graph.set_node_as_output(new_node_id, j_node.value("is_output", false));
                new_graph.set_node_position(new_node_id, j_node.value("x", 0.0f), j_node.value("y", 0.0f));
                
                if (j_node.contains("input_pins")) {
                    auto old_in_pins = j_node["input_pins"].get<std::vector<int>>();
                    for (size_t i = 0; i < old_in_pins.size() && i < new_node->input_pin_ids.size(); ++i) {
                        pin_map[old_in_pins[i]] = new_node->input_pin_ids[i];
                    }
                }
                if (j_node.contains("output_pins")) {
                    auto old_out_pins = j_node["output_pins"].get<std::vector<int>>();
                    for (size_t i = 0; i < old_out_pins.size() && i < new_node->output_pin_ids.size(); ++i) {
                        pin_map[old_out_pins[i]] = new_node->output_pin_ids[i];
                    }
                }
            }
        }
        
        for (const auto& j_link : j["links"]) {
            int old_source = j_link.value("source", -1);
            int old_dest = j_link.value("dest", -1);
            if (pin_map.count(old_source) && pin_map.count(old_dest)) {
                new_graph.add_link(pin_map[old_source], pin_map[old_dest]);
            }
        }
        
        main_graph_ = std::move(new_graph);
        
        dummy_effects_.clear();
        for (const auto& node : main_graph_.get_nodes()) {
            if (node.pedal) {
                dummy_effects_.push_back(node.pedal);
            }
        }
        
        // Push topology to audio thread without deadlocking (we already hold effect_mutex_)
        auto new_executor = std::make_shared<AudioGraphExecutor>();
        new_executor->prepare(sample_rate_, buffer_size_, 32);
        new_executor->compile(main_graph_);
        main_executor_ = new_executor;
        topology_dirty_.store(true, std::memory_order_release);
        
        return;
    }
    
    // Legacy Array Hydration
    if (j.contains("effects")) {
        for (const auto& fx_data : j["effects"]) {
            std::string name = fx_data["name"];
            for (auto& fx : dummy_effects_) {
                if (fx && std::string(fx->name()) == name) {
                    fx->set_params(fx_data["params"]);
                }
            }
        }
    }
}

// --- Existing Methods ---

void AudioEngine::set_buffer_size(int size) {
    size = std::max(MIN_BUFFER_SIZE, std::min(MAX_BUFFER_SIZE, size));
    int prev_size = buffer_size_;
    bool was_running = running_;
    if (was_running) stop();
    buffer_size_ = size;
    if (was_running) {
        if (!start()) {
            last_error_ = "Failed with buffer size " + std::to_string(size) + ". Reverting.";
            std::cerr << "[Amplitron] " << last_error_ << std::endl;
            buffer_size_ = prev_size;
            start();
        } else {
            last_error_.clear();
        }
    }
}



void AudioEngine::set_sample_rate(int rate) {
    int prev_rate = sample_rate_;
    bool was_running = running_;
    if (was_running) stop();
    sample_rate_ = rate;
    
    {
        std::lock_guard<std::mutex> lock(effect_mutex_);
        // FIX: Iterate over the nodes in the new AudioGraph
        for (const auto& node : main_graph_.get_nodes()) {
            if (node.pedal) { // Check if it's a standard effect and not a bare merge node
                node.pedal->set_sample_rate(rate);
                node.pedal->reset();
            }
        }
        if (tuner_tap_) {
            tuner_tap_->set_sample_rate(rate);
            tuner_tap_->reset();
        }
    }
    
    if (was_running) {
        if (!start()) {
            last_error_ = "Failed with sample rate " + std::to_string(rate) + " Hz. Reverting.";
            std::cerr << "[Amplitron] " << last_error_ << std::endl;
            sample_rate_ = prev_rate;
            
            std::lock_guard<std::mutex> lock(effect_mutex_);
            // FIX: Revert the sample rates using the graph nodes
            for (const auto& node : main_graph_.get_nodes()) {
                if (node.pedal) {
                    node.pedal->set_sample_rate(prev_rate);
                    node.pedal->reset();
                }
            }
            if (tuner_tap_) {
                tuner_tap_->set_sample_rate(prev_rate);
                tuner_tap_->reset();
            }
            start();
        } else {
            last_error_.clear();
        }
    }
}
void AudioEngine::commit_graph_changes() {
    std::lock_guard<std::mutex> lock(effect_mutex_);
    
    // 1. Create a brand new executor (so we don't mutate memory the audio thread is currently reading)
    auto new_executor = std::make_shared<AudioGraphExecutor>();
    new_executor->prepare(sample_rate_, buffer_size_, 32);
    
    // 2. Compile the latest UI graph into the new executor
    new_executor->compile(main_graph_);
    
    // 3. Promote it to the main slot. The audio thread will grab it on the next try_lock!
    main_executor_ = new_executor;
    topology_dirty_.store(true, std::memory_order_release);
}
}