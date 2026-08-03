// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import 'multi_view_testing.dart';

class _ScopedBuildTestBinding extends AutomatedTestWidgetsFlutterBinding {
  Set<int>? frameViewIds;

  @override
  Set<int>? get activeFrameViewIds => frameViewIds;
}

class _BuildProbe extends StatefulWidget {
  const _BuildProbe({required super.key});

  @override
  State<_BuildProbe> createState() => _BuildProbeState();
}

class _BuildProbeState extends State<_BuildProbe> {
  int buildCount = 0;

  void markDirty() {
    setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    buildCount += 1;
    return const SizedBox();
  }
}

void main() {
  final binding = _ScopedBuildTestBinding();

  testWidgets('unprocessed view scope requests another frame opportunity', (
    WidgetTester tester,
  ) async {
    final firstView = FakeView(tester.view, viewId: 501);
    final secondView = FakeView(tester.view, viewId: 502);
    final firstKey = GlobalKey<_BuildProbeState>();
    final secondKey = GlobalKey<_BuildProbeState>();
    addTearDown(() => binding.frameViewIds = null);

    await tester.pumpWidget(
      wrapWithView: false,
      ViewCollection(
        views: <Widget>[
          View(
            view: firstView,
            child: _BuildProbe(key: firstKey),
          ),
          View(
            view: secondView,
            child: _BuildProbe(key: secondKey),
          ),
        ],
      ),
    );

    final int firstBuilds = firstKey.currentState!.buildCount;
    final int secondBuilds = secondKey.currentState!.buildCount;
    firstKey.currentState!.markDirty();
    secondKey.currentState!.markDirty();

    binding.frameViewIds = <int>{firstView.viewId};
    await tester.pump();

    expect(firstKey.currentState!.buildCount, firstBuilds + 1);
    expect(secondKey.currentState!.buildCount, secondBuilds);
    expect(binding.hasScheduledFrame, isTrue);

    binding.frameViewIds = <int>{secondView.viewId};
    await tester.pump();

    expect(secondKey.currentState!.buildCount, secondBuilds + 1);
    expect(binding.hasScheduledFrame, isFalse);
  });
}
