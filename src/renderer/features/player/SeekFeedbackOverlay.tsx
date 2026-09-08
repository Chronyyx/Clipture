import { ChevronLeft, ChevronRight, Minus, Plus } from 'lucide-react';
import type { SeekFeedback } from './usePlayerInteractions';

export function SeekFeedbackOverlay({ feedback }: { feedback: SeekFeedback }) {
  return (
    <div key={feedback.sequence} className={'seek-feedback ' + (feedback.direction < 0 ? 'backward' : 'forward')} aria-hidden='true'>
      {feedback.direction < 0 && <ChevronLeft className='seek-feedback-chevron' />}
      {feedback.direction < 0 ? <Minus className='seek-feedback-sign' /> : <Plus className='seek-feedback-sign' />}
      <strong className='seek-feedback-number'>{feedback.seconds}</strong>
      {feedback.direction > 0 && <ChevronRight className='seek-feedback-chevron' />}
    </div>
  );
}
