{
  'target_defaults': {
    'variables': {
      'deps': [
        'expat',
        'libchrome-<(libbase_ver)',
        'libcrypto',
      ],
    },
    'include_dirs': [
      '.',
      '../libweave/include',
    ],
  },
  'targets': [
    {
      'target_name': 'libweave_external',
      'type': 'static_library',
      'include_dirs': [
        '../libweave/third_party/modp_b64/modp_b64/',
      ],
      'sources': [
        'external/crypto/p224.cc',
        'external/crypto/p224_spake.cc',
        'external/crypto/sha2.cc',
        'third_party/modp_b64/modp_b64.cc',
      ],
    },
    {
      'target_name': 'libweave_common',
      'type': 'static_library',
      'cflags!': ['-fPIE'],
      'cflags': ['-fPIC'],
      'sources': [
        'src/backoff_entry.cc',
        'src/base_api_handler.cc',
        'src/commands/cloud_command_proxy.cc',
        'src/commands/command_definition.cc',
        'src/commands/command_dictionary.cc',
        'src/commands/command_instance.cc',
        'src/commands/command_manager.cc',
        'src/commands/command_queue.cc',
        'src/commands/object_schema.cc',
        'src/commands/prop_constraints.cc',
        'src/commands/prop_types.cc',
        'src/commands/prop_values.cc',
        'src/commands/schema_constants.cc',
        'src/commands/schema_utils.cc',
        'src/commands/user_role.cc',
        'src/config.cc',
        'src/data_encoding.cc',
        'src/device_manager.cc',
        'src/device_registration_info.cc',
        'src/error.cc',
        'src/http_constants.cc',
        'src/json_error_codes.cc',
        'src/notification/notification_parser.cc',
        'src/notification/pull_channel.cc',
        'src/notification/xml_node.cc',
        'src/notification/xmpp_channel.cc',
        'src/notification/xmpp_iq_stanza_handler.cc',
        'src/notification/xmpp_stream_parser.cc',
        'src/privet/cloud_delegate.cc',
        'src/privet/constants.cc',
        'src/privet/device_delegate.cc',
        'src/privet/openssl_utils.cc',
        'src/privet/privet_handler.cc',
        'src/privet/privet_manager.cc',
        'src/privet/privet_types.cc',
        'src/privet/publisher.cc',
        'src/privet/security_manager.cc',
        'src/privet/wifi_bootstrap_manager.cc',
        'src/privet/wifi_ssid_generator.cc',
        'src/registration_status.cc',
        'src/states/error_codes.cc',
        'src/states/state_change_queue.cc',
        'src/states/state_manager.cc',
        'src/states/state_package.cc',
        'src/string_utils.cc',
        'src/utils.cc',
      ],
    },
    {
      'target_name': 'libweave-<(libbase_ver)',
      'type': 'shared_library',
      'includes': [
        '../common-mk/deps.gypi',
      ],
      'dependencies': [
        'libweave_common',
        'libweave_external',
      ],
      'sources': [
        'src/empty.cc',
      ],
    },
    {
      'target_name': 'libweave-test-<(libbase_ver)',
      'type': 'static_library',
      'standalone_static_library': 1,
      'sources': [
        'src/commands/mock_command.cc',
        'src/commands/unittest_utils.cc',
        'src/mock_http_client.cc',
      ],
      'includes': ['../common-mk/deps.gypi'],
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'libweave_testrunner',
          'type': 'executable',
          'variables': {
            'deps': [
              'libchrome-test-<(libbase_ver)',
            ],
          },
          'dependencies': [
            'libweave_common',
            'libweave_external',
            'libweave-test-<(libbase_ver)',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'external/crypto/p224_spake_unittest.cc',
            'external/crypto/p224_unittest.cc',
            'external/crypto/sha2_unittest.cc',
            'src/backoff_entry_unittest.cc',
            'src/base_api_handler_unittest.cc',
            'src/commands/cloud_command_proxy_unittest.cc',
            'src/commands/command_definition_unittest.cc',
            'src/commands/command_dictionary_unittest.cc',
            'src/commands/command_instance_unittest.cc',
            'src/commands/command_manager_unittest.cc',
            'src/commands/command_queue_unittest.cc',
            'src/commands/object_schema_unittest.cc',
            'src/commands/schema_utils_unittest.cc',
            'src/config_unittest.cc',
            'src/data_encoding_unittest.cc',
            'src/device_registration_info_unittest.cc',
            'src/error_unittest.cc',
            'src/notification/notification_parser_unittest.cc',
            'src/notification/xml_node_unittest.cc',
            'src/notification/xmpp_channel_unittest.cc',
            'src/notification/xmpp_iq_stanza_handler_unittest.cc',
            'src/notification/xmpp_stream_parser_unittest.cc',
            'src/privet/privet_handler_unittest.cc',
            'src/privet/security_manager_unittest.cc',
            'src/privet/wifi_ssid_generator_unittest.cc',
            'src/states/state_change_queue_unittest.cc',
            'src/states/state_manager_unittest.cc',
            'src/states/state_package_unittest.cc',
            'src/string_utils_unittest.cc',
            'src/weave_testrunner.cc',
          ],
        },
      ],
    }],
  ],
}
