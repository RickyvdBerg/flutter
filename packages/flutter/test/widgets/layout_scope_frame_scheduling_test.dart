// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import 'multi_view_testing.dart';

class _RecordingBinding extends AutomatedTestWidgetsFlutterBinding {
  final List<String> events = <String>[];

  @override
  bool dispatchPlatformScheduleFrame() {
    events.add('request');
    return true;
  }
}

class _Probe extends StatefulWidget {
  const _Probe({required super.key});

  @override
  State<_Probe> createState() => _ProbeState();
}

class _ProbeState extends State<_Probe> {
  void dirty() => setState(() {});

  @override
  Widget build(BuildContext context) => const SizedBox();
}

void main() {
  final binding = _RecordingBinding();
  testWidgets('dirty view registration precedes the first frame request', (
    WidgetTester tester,
  ) async {
    final key = GlobalKey<_ProbeState>();
    await tester.pumpWidget(
      wrapWithView: false,
      ViewCollection(
        views: <Widget>[
          View(
            view: FakeView(tester.view, viewId: 401),
            child: LayoutBuilder(builder: (context, constraints) => _Probe(key: key)),
          ),
        ],
      ),
    );
    await tester.pump();
    final owner = binding.buildOwner!;
    final previous = owner.onElementDirtied;
    owner.onElementDirtied = (BuildViewIdentity identity) {
      binding.events.add('dirty');
      previous?.call(identity);
    };
    addTearDown(() => owner.onElementDirtied = previous);
    binding.events.clear();
    key.currentState!.dirty();
    final actual = List<String>.of(binding.events);
    await tester.pump();
    expect(actual, <String>['dirty', 'request']);
  });
}
