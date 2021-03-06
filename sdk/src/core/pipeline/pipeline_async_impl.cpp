// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2016 Intel Corporation. All Rights Reserved.

#include <algorithm>
#include <exception>
#include "rs/utils/librealsense_conversion_utils.h"
#include "rs/utils/log_utils.h"
#include "pipeline_async_impl.h"
#include "sync_samples_consumer.h"
#include "async_samples_consumer.h"
#include "config_util.h"

using namespace std;
using namespace rs::utils;

namespace rs
{
    namespace core
    {
        pipeline_async_impl::pipeline_async_impl() :
            m_current_state(state::unconfigured),
            m_user_requested_time_sync_mode(video_module_interface::supported_module_config::time_sync_mode::sync_not_required),
            m_device_manager(nullptr),
            m_context(new context()) { }

        pipeline_async_impl::pipeline_async_impl(const pipeline_async::testing_mode mode,
                                                 const char * file_path) : pipeline_async_impl()
        {
            try
            {
                switch (mode) {
                case pipeline_async::testing_mode::playback:
                    // initiate context from a playback file
                    m_context.reset(new rs::playback::context(file_path));
                    break;
                case pipeline_async::testing_mode::record:
                    // initiate context as a recording device
                    m_context.reset(new rs::record::context(file_path));
                    break;
                }
            }
            catch(const std::exception & ex)
            {
                std::throw_with_nested(std::runtime_error("failed to create context"));
            }
        }

        status pipeline_async_impl::add_cv_module(video_module_interface * cv_module)
        {
            if(!cv_module)
            {
                return status_data_not_initialized;
            }

            std::lock_guard<std::mutex> state_guard(m_state_lock);
            switch(m_current_state)
            {
                case state::streaming:
                case state::configured:
                    return status_invalid_state;
                case state::unconfigured:
                default:
                    break;
            }

            if(std::find_if(m_cv_modules.begin(), m_cv_modules.end(),
                            [cv_module] (video_module_interface * checked_module)-> bool
                                {
                                    return cv_module == checked_module;
                                }
                            ) != m_cv_modules.end())
            {
                return status_param_inplace;
            }

            m_cv_modules.push_back(cv_module);
            return status_no_error;
        }

        status pipeline_async_impl::query_cv_module(uint32_t index, video_module_interface ** cv_module) const
        {
            std::lock_guard<std::mutex> state_guard(m_state_lock);

            if(m_cv_modules.size() <= index)
            {
                return status_value_out_of_range;
            }

            if(cv_module == nullptr)
            {
                return status_handle_invalid;
            }

            *cv_module = m_cv_modules.at(index);
            return status_no_error;
        }

        status pipeline_async_impl::query_default_config(uint32_t index, video_module_interface::supported_module_config & default_config) const
        {
            //currently supporting a single hardcoded config
            if(index != 0)
            {
                return status_value_out_of_range;
            }

            default_config = get_hardcoded_superset_config();
            return status_no_error;
        }

        status pipeline_async_impl::set_config(const video_module_interface::supported_module_config &config)
        {
            std::lock_guard<std::mutex> state_guard(m_state_lock);
            switch(m_current_state)
            {
                case state::streaming:
                    return status_invalid_state;
                case state::configured:
                case state::unconfigured:
                default:
                    break;
            }
            auto set_config_status = set_config_unsafe(config);
            if(set_config_status == status_no_error)
            {
                m_current_state = state::configured;
            }
            return set_config_status;
        }

        status pipeline_async_impl::query_current_config(video_module_interface::actual_module_config & current_config) const
        {
            std::lock_guard<std::mutex> state_guard(m_state_lock);
            switch(m_current_state)
            {
                case state::unconfigured:
                    return status_invalid_state;
                case state::configured:
                case state::streaming:
                default:
                    break;
            }
            if(!m_device_manager)
            {
                return status_data_unavailable;
            }

            m_device_manager->query_current_config(current_config);
            return status_no_error;
        }

        status pipeline_async_impl::start(callback_handler * app_callbacks_handler)
        {   
            std::lock_guard<std::mutex> state_guard(m_state_lock);
            switch(m_current_state)
            {
                case state::streaming:
                    return status_invalid_state;
                case state::unconfigured:
                {
                    auto set_config_status = set_config_unsafe({});
                    if(set_config_status < status_no_error)
                    {
                        LOG_ERROR("failed to set configuration, error code" << set_config_status);
                        return set_config_status;
                    }
                    else
                    {
                        m_current_state = state::configured;
                    }
                    break;
                }
                case state::configured:
                default:
                    break;
            }

            assert(m_current_state == state::configured && "the pipeline must be in configured state to start");
            assert(m_device_manager != nullptr && "on configured state the device manager must exist");

            std::vector<std::shared_ptr<samples_consumer_base>> samples_consumers;
            if(app_callbacks_handler)
            {
                video_module_interface::actual_module_config actual_pipeline_config = {};
                m_device_manager->query_current_config(actual_pipeline_config);
                //application samples consumer creation :
                samples_consumers.push_back(std::unique_ptr<samples_consumer_base>(
                    new sync_samples_consumer(
                            [app_callbacks_handler](std::shared_ptr<correlated_sample_set> sample_set)
                                {
                                  app_callbacks_handler->on_new_sample_set(*sample_set);
                                },
                            actual_pipeline_config,
                            m_user_requested_time_sync_mode)));
            }
            // create a samples consumer for each cv module
            for(auto cv_module : m_cv_modules)
            {
                video_module_interface::actual_module_config & actual_module_config = std::get<0>(m_modules_configs[cv_module]);
                bool is_cv_module_async = std::get<1>(m_modules_configs[cv_module]);
                video_module_interface::supported_module_config::time_sync_mode module_time_sync_mode = std::get<2>(m_modules_configs[cv_module]);
                if(is_cv_module_async)
                {
                    samples_consumers.push_back(std::unique_ptr<samples_consumer_base>(new async_samples_consumer(
                                                                                               app_callbacks_handler,
                                                                                               cv_module,
                                                                                               actual_module_config,
                                                                                               module_time_sync_mode)));
                }
                else //cv_module is sync
                {
                    samples_consumers.push_back(std::unique_ptr<samples_consumer_base>(new sync_samples_consumer(
                            [cv_module, app_callbacks_handler](std::shared_ptr<correlated_sample_set> sample_set)
                            {
                                //push to sample_set to the cv module
                                auto status = cv_module->process_sample_set(*sample_set);

                                if(status < status_no_error)
                                {
                                    LOG_ERROR("cv module failed to sync process sample set, error code" << status);
                                    if(app_callbacks_handler)
                                    {
                                        app_callbacks_handler->on_error(status);
                                    }
                                    return;
                                }
                                if(app_callbacks_handler)
                                {
                                    app_callbacks_handler->on_cv_module_process_complete(cv_module);
                                }
                            },
                            actual_module_config,
                            module_time_sync_mode)));
                }
            }

            try
            {
                m_device_manager->start();
            }
            catch(const std::exception & ex)
            {
                LOG_ERROR("failed to start device, error message : " << ex.what());
                return status_device_failed;
            }
            catch(...)
            {
                LOG_ERROR("failed to start device");
                return status_device_failed;
            }

            //commit to update the pipeline state
            {
                std::lock_guard<std::mutex> samples_consumers_guard(m_samples_consumers_lock);
                m_samples_consumers = std::move(samples_consumers);
            }

            m_current_state = state::streaming;
            return status_no_error;
        }

        status pipeline_async_impl::stop()
        {
            std::lock_guard<std::mutex> state_guard(m_state_lock);
            switch(m_current_state)
            {
                case state::streaming:
                    break;
                case state::configured:
                case state::unconfigured:
                default:
                    return status_invalid_state;
            }

            ordered_resources_reset();
            m_current_state = state::configured;
            return status_no_error;
        }

        status pipeline_async_impl::reset()
        {
            std::lock_guard<std::mutex> state_guard(m_state_lock);
            ordered_resources_reset();
            m_device_manager.reset();
            m_cv_modules.clear();
            m_modules_configs.clear();
            m_user_requested_time_sync_mode = video_module_interface::supported_module_config::time_sync_mode::sync_not_required;
            m_current_state = state::unconfigured;
            return status_no_error;
        }

        rs::device * pipeline_async_impl::get_device()
        {
            if(!m_device_manager)
            {
                return nullptr;
            }
            return m_device_manager->get_underlying_device();
        }

        rs::device * pipeline_async_impl::get_device_from_config(const video_module_interface::supported_module_config & config) const
        {
            auto device_count = m_context->get_device_count();
            auto is_any_device_valid = (std::strcmp(config.device_name, "") == 0);
            for(int i = 0; i < device_count; ++i)
            {
                auto device = m_context->get_device(i);
                if(is_any_device_valid || std::strcmp(config.device_name, device->get_name()) == 0)
                {
                    return device;
                }
            }
            return nullptr;
        }

        bool pipeline_async_impl::is_there_a_satisfying_module_config(video_module_interface * cv_module,
                                                                      const video_module_interface::supported_module_config & given_config,
                                                                      video_module_interface::supported_module_config & satisfying_config) const
        {
            for(auto config_index = 0;; ++config_index)
            {
                video_module_interface::supported_module_config supported_config = {};
                if(cv_module->query_supported_module_config(config_index, supported_config) < status_no_error)
                {
                    //finished looping through the supported configs and haven't found a satisfying config
                    return false;
                }

                auto is_the_device_in_the_current_config_valid = std::strlen(given_config.device_name) == 0 ||
                                                                 (std::strcmp(given_config.device_name, supported_config.device_name) == 0);
                        ;
                if (!is_the_device_in_the_current_config_valid)
                {
                    //skip config due to miss-matching the given config device
                    continue;
                }

                bool are_all_streams_in_the_current_config_satisfied = true;
                for(uint32_t stream_index = 0; stream_index < static_cast<uint32_t>(stream_type::max); ++stream_index)
                {
                    video_module_interface::supported_image_stream_config & stream_config = supported_config.image_streams_configs[stream_index];
                    if(stream_config.is_enabled)
                    {
                        const video_module_interface::supported_image_stream_config & given_stream_config = given_config.image_streams_configs[stream_index];
                        //compare the stream with the given config
                        bool is_satisfying_stream_resolution = stream_config.size.width == given_stream_config.size.width &&
                                                               stream_config.size.height == given_stream_config.size.height;
                        bool is_satisfying_stream_framerate =  stream_config.frame_rate == given_stream_config.frame_rate ||
                                                               stream_config.frame_rate == 0;
                        bool is_satisfying_stream_config = given_stream_config.is_enabled &&
                                                           is_satisfying_stream_resolution &&
                                                           is_satisfying_stream_framerate;

                        if(!is_satisfying_stream_config)
                        {
                            are_all_streams_in_the_current_config_satisfied = false;
                            break; //out of the streams loop
                        }
                    }
                }
                if(!are_all_streams_in_the_current_config_satisfied)
                {
                    continue; // the current supported config is not satisfying all streams, try the next one
                }

                bool are_all_motions_in_the_current_config_satisfied = true;
                for(uint32_t motion_index = 0; motion_index < static_cast<uint32_t>(motion_type::max); ++motion_index)
                {
                    video_module_interface::supported_motion_sensor_config & motion_config = supported_config.motion_sensors_configs[motion_index];

                    if(motion_config.is_enabled)
                    {
                        const video_module_interface::supported_motion_sensor_config & given_motion_config = given_config.motion_sensors_configs[motion_index];

                        if(!given_motion_config.is_enabled)
                        {
                            are_all_motions_in_the_current_config_satisfied = false;
                            break;
                        }
                    }
                }
                if(!are_all_motions_in_the_current_config_satisfied)
                {
                    continue; // the current supported config is not satisfying all motions, try the next one
                }

                // found satisfying config
                satisfying_config = supported_config;
                return true;
            }

            return false;
        }

        void pipeline_async_impl::non_blocking_sample_callback(std::shared_ptr<correlated_sample_set> sample_set)
        {
            std::lock_guard<std::mutex> samples_consumers_guard(m_samples_consumers_lock);
            for(size_t i = 0; i < m_samples_consumers.size(); ++i)
            {
                m_samples_consumers[i]->notify_sample_set_non_blocking(sample_set);
            }
        }

        void pipeline_async_impl::ordered_resources_reset()
        {
            //the order of destruction is critical,
            //the consumers must release all resources allocated by the device inorder to stop and release the device.
            {
                std::lock_guard<std::mutex> samples_consumers_guard(m_samples_consumers_lock);
                m_samples_consumers.clear();
            }

            // cv modules reset
            for (auto cv_module : m_cv_modules)
            {
                cv_module->flush_resources();
            }

            if(m_device_manager)
            {
                m_device_manager->stop();
            }
        }

        const video_module_interface::supported_module_config pipeline_async_impl::get_hardcoded_superset_config() const
        {
            video_module_interface::supported_module_config hardcoded_config = {};

            hardcoded_config.samples_time_sync_mode = video_module_interface::supported_module_config::time_sync_mode::sync_not_required;

            video_module_interface::supported_image_stream_config & depth_desc = hardcoded_config[stream_type::depth];
            depth_desc.size.width = 640;
            depth_desc.size.height = 480;
            depth_desc.frame_rate = 30;
            depth_desc.flags = sample_flags::none;
            depth_desc.is_enabled = true;

            video_module_interface::supported_image_stream_config & color_desc = hardcoded_config[stream_type::color];
            color_desc.size.width = 640;
            color_desc.size.height = 480;
            color_desc.frame_rate = 30;
            color_desc.flags = sample_flags::none;
            color_desc.is_enabled = true;

            video_module_interface::supported_image_stream_config & ir_desc = hardcoded_config[stream_type::infrared];
            ir_desc.size.width = 640;
            ir_desc.size.height = 480;
            ir_desc.frame_rate = 30;
            ir_desc.flags = sample_flags::none;
            ir_desc.is_enabled = true;

            video_module_interface::supported_image_stream_config & ir2_desc = hardcoded_config[stream_type::infrared2];
            ir2_desc.size.width = 640;
            ir2_desc.size.height = 480;
            ir2_desc.frame_rate = 30;
            ir2_desc.flags = sample_flags::none;
            ir2_desc.is_enabled = true;

            video_module_interface::supported_image_stream_config & fisheye_desc = hardcoded_config[stream_type::fisheye];
            fisheye_desc.size.width = 640;
            fisheye_desc.size.height = 480;
            fisheye_desc.frame_rate = 30;
            fisheye_desc.flags = sample_flags::none;
            fisheye_desc.is_enabled = true;

            video_module_interface::supported_motion_sensor_config & accel_desc = hardcoded_config[motion_type::accel];
            accel_desc.flags = sample_flags::none;
            accel_desc.sample_rate = 250;
            accel_desc.is_enabled = true;

            video_module_interface::supported_motion_sensor_config & gyro_desc = hardcoded_config[motion_type::gyro];
            gyro_desc.flags = sample_flags::none;
            gyro_desc.sample_rate = 200;
            gyro_desc.is_enabled = true;

            return hardcoded_config;
        }

        status pipeline_async_impl::set_config_unsafe(const video_module_interface::supported_module_config & config)
        {
            if(config_util::is_config_empty(config) && m_cv_modules.empty())
            {
                return status::status_invalid_argument;
            }

            //pull the modules configurations
            vector<vector<video_module_interface::supported_module_config>> groups;
            for (auto cv_module : m_cv_modules)
            {
                vector<video_module_interface::supported_module_config> configs;
                for(uint32_t config_index = 0;; config_index++)
                {
                    video_module_interface::supported_module_config module_config = {};
                    if(cv_module->query_supported_module_config(config_index, module_config) < status_no_error)
                    {
                        break;
                    }
                    configs.push_back(module_config);
                }

                groups.push_back(configs);
            }

            //add the user's config as a configuration restriction
            groups.push_back({config});

            //generate flatten supersets from the grouped configurations
            vector<video_module_interface::supported_module_config> supersets;
            config_util::generete_matching_supersets(groups, supersets);

            //try to set each superset on the device and the modules
            for(auto & superset : supersets)
            {
                m_device_manager.reset();

                std::unique_ptr<rs::core::device_manager> device_manager;
                try
                {
                    device_manager.reset(new rs::core::device_manager(get_device_from_config(superset),
                                                                      superset,
                                                                      [this](std::shared_ptr<correlated_sample_set> sample_set) { non_blocking_sample_callback(sample_set); }));
                }
                catch(const std::runtime_error & ex)
                {
                    LOG_INFO("skipping config that failed to set the device : " << ex.what());
                    continue;
                }
                catch(...)
                {
                    LOG_INFO("skipping config that failed to set the device");
                    continue;
                }

                std::map<video_module_interface *, std::tuple<video_module_interface::actual_module_config,
                                                              bool,
                                                              video_module_interface::supported_module_config::time_sync_mode>> modules_configs;
                bool found_satisfying_config_to_each_module = true;
                //get satisfying modules configurations
                for (auto cv_module : m_cv_modules)
                {
                    video_module_interface::supported_module_config satisfying_config = {};
                    if(is_there_a_satisfying_module_config(cv_module, superset, satisfying_config))
                    {
                        auto actual_module_config = device_manager->create_actual_config_from_supported_config(satisfying_config);

                        //save the module configuration
                        modules_configs[cv_module] = std::make_tuple(actual_module_config, satisfying_config.async_processing, satisfying_config.samples_time_sync_mode);
                    }
                    else
                    {
                        LOG_ERROR("no available configuration for module id : " << cv_module->query_module_uid());
                        found_satisfying_config_to_each_module = false;
                        break;
                    }
                }

                if(!found_satisfying_config_to_each_module)
                {
                    continue; //check next config
                }

                //set the satisfying modules configurations
                status module_config_status = status_no_error;
                for (auto cv_module : m_cv_modules)
                {
                    auto & actual_module_config = std::get<0>(modules_configs[cv_module]);
                    auto status = cv_module->set_module_config(actual_module_config);
                    if(status < status_no_error)
                    {
                        LOG_ERROR("failed to set configuration on module id : " << cv_module->query_module_uid());
                        module_config_status = status;
                        break;
                    }
                }

                //if there was a failure to set one of modules, fallback by resetting all modules and disabling the device streams
                if(module_config_status < status_no_error)
                {
                    //clear the configured modules
                    for (auto cv_module : m_cv_modules)
                    {
                        cv_module->reset_config();
                    }

                    continue; //check next config
                }

                //commit updated config
                m_modules_configs.swap(modules_configs);
                m_device_manager = std::move(device_manager);
                m_user_requested_time_sync_mode = config.samples_time_sync_mode;
                return status_no_error;
            }

            return status_match_not_found;
        }

        pipeline_async_impl::~pipeline_async_impl()
        {
            ordered_resources_reset();
        }
    }
}

