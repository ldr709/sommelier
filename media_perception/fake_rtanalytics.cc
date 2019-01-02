// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media_perception/fake_rtanalytics.h"

#include <string>
#include <vector>

#include "media_perception/media_perception_mojom.pb.h"
#include "media_perception/proto_mojom_conversion.h"
#include "media_perception/serialized_proto.h"

namespace mri {

void FakeRtanalytics::SetSerializedDeviceTemplates(
    std::vector<SerializedDeviceTemplate> serialized_device_templates) {
  serialized_device_templates_ = serialized_device_templates;
}

std::vector<PerceptionInterfaceType> FakeRtanalytics::SetupConfiguration(
    const std::string& configuration_name,
    SerializedSuccessStatus* success_status) {
  SuccessStatus status;
  status.set_success(true);
  status.set_failure_reason(configuration_name);
  *success_status = Serialized<SuccessStatus>(status).GetBytes();
  std::vector<PerceptionInterfaceType> interface_types;
  interface_types.push_back(PerceptionInterfaceType::INTERFACE_TYPE_UNKNOWN);
  return interface_types;
}

std::vector<SerializedDeviceTemplate> FakeRtanalytics::GetTemplateDevices(
      const std::string& configuration_name) const {
  return serialized_device_templates_;
}

SerializedSuccessStatus FakeRtanalytics::SetVideoDeviceForTemplateName(
    const std::string& configuration_name, const std::string& template_name,
    const SerializedVideoDevice& video_device) {
  SuccessStatus status;
  status.set_success(true);
  status.set_failure_reason(template_name);
  return Serialized<SuccessStatus>(status).GetBytes();
}

SerializedSuccessStatus FakeRtanalytics::SetAudioDeviceForTemplateName(
    const std::string& configuration_name, const std::string& template_name,
    const SerializedAudioDevice& audio_device) {
  SuccessStatus status;
  status.set_success(true);
  status.set_failure_reason(template_name);
  return Serialized<SuccessStatus>(status).GetBytes();
}

SerializedSuccessStatus FakeRtanalytics::SetVirtualVideoDeviceForTemplateName(
    const std::string& configuration_name, const std::string& template_name,
    const SerializedVirtualVideoDevice& virtual_device) {
  SuccessStatus status;
  status.set_success(true);
  status.set_failure_reason(template_name);
  return Serialized<SuccessStatus>(status).GetBytes();
}

SerializedPipelineState FakeRtanalytics::GetPipelineState(
    const std::string& configuration_name) const {
  PipelineState pipeline_state;
  pipeline_state.set_status(PipelineStatus::SUSPENDED);
  return Serialized<PipelineState>(pipeline_state).GetBytes();
}

SerializedPipelineState FakeRtanalytics::SetPipelineState(
      const std::string& configuration_name,
      const SerializedPipelineState& desired_state) {
  PipelineState pipeline_state = Serialized<PipelineState>(
      desired_state).Deserialize();
  return Serialized<PipelineState>(pipeline_state).GetBytes();
}

}  // namespace mri
