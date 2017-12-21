#!/usr/bin/env python2
# Copyright 2017 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Transforms and validates cros config from source YAML to target JSON"""

from __future__ import print_function

import argparse
import collections
import json
from jsonschema import validate
import os
import sys
import yaml

this_dir = os.path.dirname(__file__)

MODELS = 'models'
BUILD_ONLY_ELEMENTS = [
  '/firmware',
  '/audio/main/card',
  '/audio/main/cras-config-subdir',
  '/audio/main/files'
]
CRAS_CONFIG_DIR = '/etc/cras'

def GetNamedTuple(mapping):
  """Converts a mapping into Named Tuple recursively.

  Args:
    mapping: A mapping object to be converted.

  Returns:
    A named tuple generated from mapping
  """
  if not isinstance(mapping, collections.Mapping):
    return mapping
  new_mapping = {}
  for k, v in mapping.iteritems():
    if type(v) is list:
      new_list = []
      for val in v:
        new_list.append(GetNamedTuple(val))
      new_mapping[k.replace('-', '_').replace('@', '_')] = new_list
    else:
      new_mapping[k.replace('-', '_').replace('@', '_')] = GetNamedTuple(v)
  return collections.namedtuple('Config', new_mapping.iterkeys())(**new_mapping)

def ParseArgs(argv):
  """Parse the available arguments.

  Invalid arguments or -h cause this function to print a message and exit.

  Args:
    argv: List of string arguments (excluding program name / argv[0])

  Returns:
    argparse.Namespace object containing the attributes.
  """
  parser = argparse.ArgumentParser(
      description='Validates a YAML cros-config and transforms it to JSON')
  parser.add_argument(
      '-s',
      '--schema',
      type=str,
      help='Path to the schema file used to validate the config')
  parser.add_argument(
      '-c',
      '--config',
      type=str,
      help='Path to the config file (YAML) that will be validated/transformed')
  parser.add_argument(
      '-o',
      '--output',
      type=str,
      help='Output file that will be generated by the transform (system file)')
  parser.add_argument(
      '-f',
      '--filter',
      type=bool,
      default=False,
      help='Filter build specific elements from the output JSON')
  return parser.parse_args(argv)

def TransformConfig(config):
  """Transforms the source config (YAML) to the target system format (JSON)

  Applies consistent transforms to covert a source YAML configuration into
  JSON output that will be used on the system by cros_config.

  Args:
    config: Config that will be transformed.

  Returns:
    Resulting JSON output from the transform.
  """
  config_yaml = yaml.load(config)
  json_from_yaml = json.dumps(config_yaml, sort_keys=True, indent=2)
  json_config = json.loads(json_from_yaml)
  # Drop everything except for models since they were just used as shared
  # config in the source yaml.
  json_config = {MODELS: json_config[MODELS]}

  # For now, this reaches parity with the --abspath option on cros_config,
  # except it does it at build time.
  # We may standardize this, but for now doing it in the transform works.
  cras_config_dir_name = 'cras-config-dir'
  cras_config_subdir_name = 'cras-config-subdir'
  for model in json_config[MODELS]:
    audio = model['audio']['main']
    main_dir = audio.get(cras_config_dir_name, CRAS_CONFIG_DIR)
    sub_dir = audio.get(cras_config_subdir_name, None)
    if sub_dir:
      main_dir = '%s/%s' % (main_dir, sub_dir)
    audio[cras_config_dir_name] = main_dir
    audio['files'] = _GetAudioFiles(model)
    model['firmware']['bcs-uris'] = _GetFirmwareUris(model)

  return json.dumps(json_config, sort_keys=True, indent=2)

def _GetAudioFiles(model_dict):
  """Get all of the audio files to the specific model

  Args:
    model_dict: All attributes of the model

  Returns:
    List of files with {source, dest} tuples
  """
  files = []
  model = GetNamedTuple(model_dict)
  def _AddAudioFile(source, dest, file):
    files.append(
        {'source': os.path.join(source, file),
         'dest': os.path.join(dest, file)})
  card = model.audio.main.card
  subdir = model.audio.main.cras_config_subdir
  subdir_path = '%s/' % subdir if subdir else ''

  cras_source = 'cras-config'
  cras_dest = CRAS_CONFIG_DIR

  _AddAudioFile(cras_source, cras_dest, '%s%s' % (subdir_path, card))
  _AddAudioFile(cras_source, cras_dest, '%s%s' % (subdir_path, 'dsp.ini'))

  ucm_source = 'ucm-config'
  ucm_dest = '/ucm/share/alsa/ucm'
  ucm_suffix = getattr(model.audio.main, 'ucm-suffix', None)
  ucm_suffix_path = '.%s' % ucm_suffix if ucm_suffix else ''

  ucm_card_path = '%s%s' % (card, ucm_suffix_path)
  _AddAudioFile(
      ucm_source, ucm_dest, os.path.join(ucm_card_path, 'HiFi.conf'))
  _AddAudioFile(
      ucm_source,
      ucm_dest,
      os.path.join(ucm_card_path, '%s.conf' % ucm_card_path))

  if getattr(model.audio.main, 'firmware_bin', None):
    _AddAudioFile(
        'topology', '/lib/firmware', model.audio.main.firmware_bin)

  return files

def _GetFirmwareUris(model_dict):
  """Returns a list of (string) firmware URIs.

  Generates and returns a list of firmware URIs for this model. These URIs
  can be used to pull down remote firmware packages.

  Returns:
    A list of (string) full firmware URIs, or an empty list on failure.
  """

  model = GetNamedTuple(model_dict)
  fw = model.firmware
  fw_dict = model.firmware._asdict()

  if not getattr(fw, 'bcs_overlay'):
    return []
  bcs_overlay = fw.bcs_overlay.replace('overlay-', '')
  base_model = fw.build_targets.coreboot

  valid_images = [p for n, p in fw_dict.iteritems()
                  if n.endswith('image') and p]
  uri_format = ('gs://chromeos-binaries/HOME/bcs-{bcs}/overlay-{bcs}/'
                'chromeos-base/chromeos-firmware-{base_model}/{fname}')
  return [uri_format.format(bcs=bcs_overlay, model=model.name, fname=fname,
                            base_model=base_model)
          for fname in valid_images]

def FilterBuildElements(config):
  """Removes build only elements from the schema.

  Removes build only elements from the schema in preparation for the platform.

  Args:
    config: Config (transformed) that will be filtered
  """
  json_config = json.loads(config)
  for model in json_config[MODELS]:
    _FilterBuildElements(model, "")

  return json.dumps(json_config, sort_keys=True, indent=2)

def _FilterBuildElements(config, path):
  """Recursively checks and removes build only elements.

  Args:
    config: Dict that will be checked.
  """
  to_delete = []
  for key in config:
    full_path = "%s/%s" % (path, key)
    if full_path in BUILD_ONLY_ELEMENTS:
      to_delete.append(key)
    elif isinstance(config[key], dict):
      _FilterBuildElements(config[key], full_path)

  for key in to_delete:
    config.pop(key)

def ValidateConfigSchema(schema, config):
  """Validates a transformed cros config against the schema specified

  Verifies that the config complies with the schema supplied.

  Args:
    schema: Source schema used to verify the config.
    config: Config (transformed) that will be verified.
  """
  json_config = json.loads(config)
  schema_yaml = yaml.load(schema)
  schema_json_from_yaml = json.dumps(schema_yaml, sort_keys=True, indent=2)
  schema_json = json.loads(schema_json_from_yaml)
  validate(json_config, schema_json)


class ValidationError(Exception):
  """Exception raised for a validation error"""
  pass


def ValidateConfig(config):
  """Validates a transformed cros config for general business rules.

  Performs name uniqueness checks and any other validation that can't be
  easily performed using the schema.

  Args:
    config: Config (transformed) that will be verified.
  """
  json_config = json.loads(config)
  model_names = [model['name'] for model in json_config['models']]
  if len(model_names) != len(set(model_names)):
    raise ValidationError("Model names are not unique: %s" % model_names)


def Main(schema, config, output, filter_build_details=False):
  """Transforms and validates a cros config file for use on the system

  Applies consistent transforms to covert a source YAML configuration into
  a JSON file that will be used on the system by cros_config.

  Verifies that the file complies with the schema verification rules and
  performs additional verification checks for config consistency.

  Args:
    schema: Schema file used to verify the config.
    config: Config file that will be verified.
    output: Output file that will be generated by the transform.
    filter_build_details: Whether build only details should be filtered or not.
  """
  if not schema:
    schema = os.path.join(this_dir, 'cros_config_schema.yaml')

  with open(config, 'r') as config_stream:
    json_transform = TransformConfig(config_stream.read())
    with open(schema, 'r') as schema_stream:
      ValidateConfigSchema(schema_stream.read(), json_transform)
      ValidateConfig(json_transform)
      if filter_build_details:
        json_transform = FilterBuildElements(json_transform)
    if output:
      with open(output, 'w') as output_stream:
        output_stream.write(json_transform)
    else:
      print (json_transform)

def main(argv=None):
  args = ParseArgs(sys.argv[1:])
  Main(args.schema, args.config, args.output, args.filter)

if __name__ == "__main__":
  main()
